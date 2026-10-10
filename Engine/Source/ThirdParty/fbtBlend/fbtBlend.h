/*
-------------------------------------------------------------------------------
    This file is part of FBT (File Binary Tables).
    http://gamekit.googlecode.com/

    Copyright (c) 2010 Charlie C & Erwin Coumans.

-------------------------------------------------------------------------------
  This software is provided 'as-is', without any express or implied
  warranty. In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
-------------------------------------------------------------------------------
*/
/*
    Slightly modified by Flix01 (same license) in the following way:
    -> Now fbtBlend.h is header-only (needs FBTBLEND_IMPLEMENTATION before including it in a single .cpp file).
    ->     So now only fbtBlend.h and Blender.h can be used to parse a .blend file.
    -> Updated Blender Version to 279 (64bit, little endian).
    -> Removed all the compilation warning gcc could find.
    -> FBT_HAVE_CONFIG now has its meaning reversed: if you want to include "fbtCnfig.h" you must define it.
    -> Added int parse(const char* path);
    -> Remember to #define FBT_USE_GZ_FILE 1, if you want to support PM_COMPRESSED .blend files (you need to link to zlib: -lz).
    -> Added workaround for people just defining FBT_USE_GZ_FILE, without setting it to 1.
    -> In fbtFile, the method(s) to parse from file are:
       -> Before:
       int parse(const char* path, int mode = PM_UNCOMPRESSED);
       -> Now:
       int parse(const char* path,const char* uncompressedFileDetectorPrefix="BLENDER");
       int parse(const char* path, int mode);
       // This allows auto-detection of the .blend file compression, by just using: parse(const char* path);
     -> Added a public static helper method fbtFile::UTF8_fopen(...) (used internally to to allow UTF8 char paths on Windows).
     -> Minor changes made to class fbtArray<...> and its iterator (but code is still backward-compatible).
     -> Added class fbtString (not used here), as a replacement for std::string.

     -> Added support for zstd decompression through #define FBT_USE_ZSTD (it needs linking to -lzstd).
     -> Updated the "ID enums" (please search: [DNA_ID_enums 1/2] and [DNA_ID_enums 2/2]).
*/
#ifndef _fbtBlend_h_
#define _fbtBlend_h_

//#include "fbtFile.h"
#ifndef _fbtFile_h_
#define _fbtFile_h_

// #include "fbtTypes.h"
#ifndef _fbtTypes_h_
#define _fbtTypes_h_

#ifndef FBT_HAVE_CONFIG
// global config settings
//#define FBT_USE_GZ_FILE 1           // Adds support to PM_COMPRESSED .blend < 3.00 files (needs linking to zlib: -lz)
//#define FBT_USE_ZSTD_FILE 1         // Adds support to PM_COMPRESSED .blend 3.00 files (needs linking to zlib: -lzstd)
#define fbtDEBUG        1           // Traceback detail
//#define FBT_TYPE_LEN_VALIDATE   1   // Write a validation file (use MakeFBT.cmake->ADD_FBT_VALIDATOR to add a self validating build)
// global config settings end
#else
#include "fbtConfig.h"
#endif

// Workaround for people just defining FBT_USE_GZ_FILE, without setting it to 1
#ifdef FBT_USE_GZ_FILE
#   undef FBT_USE_GZ_FILE
#   define FBT_USE_GZ_FILE 1
#endif
// Workaround for people just defining FBT_USE_ZSTD_FILE, without setting it to 1
#ifdef FBT_USE_ZSTD_FILE
#   undef FBT_USE_ZSTD_FILE
#   define FBT_USE_ZSTD_FILE 1
#endif

#include <string.h> //memcmp

#include "Blender.h"	// THIS FILE DEPENDS ON THE BLENDER VERSION (TOGETHER WITH bfBlender.cpp THAT'S AT THE BOTTOM OF THIS FILE)

// global config settings
#ifndef fbtMaxTable
#	if BLENDER_VERSION<306
#   	define fbtMaxTable     5000        // Maximum number of elements in a table
#	else
#   	define fbtMaxTable     6000        // Maximum number of elements in a table
#	endif
#endif
#ifndef fbtMaxID
#   define fbtMaxID        64          // Maximum character array length
#endif
#ifndef fbtMaxMember
#	define fbtMaxMember    256         // Maximum number of members in a struct or class.
#endif
#ifndef fbtDefaultAlloc
#   define fbtDefaultAlloc 2048        // Table default allocation size
#endif
#ifndef FBT_ARRAY_SLOTS
#   if BLENDER_VERSION>279
#       define FBT_ARRAY_SLOTS         3   // Maximum dimensional array, eg: (int m_member[..][..] -> [FBT_ARRAY_SLOTS])
#   else
#       define FBT_ARRAY_SLOTS         2   // Maximum dimensional array, eg: (int m_member[..][..] -> [FBT_ARRAY_SLOTS])
#   endif
#endif
// global config settings end


/** \addtogroup FBT
*  @{
*/

#if (defined(DEBUG) || defined(_DEBUG)) && FBT_USE_DEBUG_ASSERTS == 1
# include <assert.h> // Keep this the only std include in headers
# define FBT_DEBUG 1
# define FBT_ASSERT(x) assert(x)
#else
# define FBT_ASSERT(x) ((void)0)
#endif

#ifdef FBT_USE_COMPILER_CHECKS
#define FBT_ASSERTCOMP(x, n) typedef bool x[(n) ? 1 : -1];
#else
#define FBT_ASSERTCOMP(x, n)
#endif


#define FBT_PLATFORM_WIN32    0
#define FBT_PLATFORM_LINUX    2
#define FBT_PLATFORM_APPLE    3

#if defined (_WIN32)
# define FBT_PLATFORM FBT_PLATFORM_WIN32
#elif defined(__APPLE__)
# define FBT_PLATFORM FBT_PLATFORM_APPLE
#else
# define FBT_PLATFORM FBT_PLATFORM_LINUX
#endif

#define FBT_COMPILER_MSVC    0
#define FBT_COMPILER_GNU     1

#if defined(_MSC_VER)
# define FBT_COMPILER FBT_COMPILER_MSVC
#elif defined(__GNUC__)
# define FBT_COMPILER FBT_COMPILER_GNU
#else
# error unknown compiler
#endif

#define FBT_ENDIAN_LITTLE    0
#define FBT_ENDIAN_BIG       1

#if defined(__sgi)      ||  defined (__sparc)        || \
    defined (__sparc__) ||  defined (__PPC__)        || \
    defined (__ppc__)   ||  defined (__BIG_ENDIAN__)
#define FBT_ENDIAN FBT_ENDIAN_BIG
#else
#define FBT_ENDIAN FBT_ENDIAN_LITTLE
#endif

#if FBT_ENDIAN == FBT_ENDIAN_BIG
# define FBT_ID(a,b,c,d) ( (int)(a)<<24 | (int)(b)<<16 | (c)<<8 | (d) )
# define FBT_ID2(c, d)   ( (c)<<8 | (d) )
#else
# define FBT_ID(a,b,c,d) ( (int)(d)<<24 | (int)(c)<<16 | (b)<<8 | (a) )
# define FBT_ID2(c, d)   ( (d)<<8 | (c) )
#endif

#define FBT_ARCH_32 0
#define FBT_ARCH_64 1

#if defined(__x86_64__)     || defined(_M_X64)      || \
    defined(__powerpc64__)  || defined(__alpha__)   || \
    defined(__ia64__)       || defined(__s390__)    || \
    defined(__s390x__)	    || defined(__aarch64__)
#define FBT_ARCH FBT_ARCH_64
#else
#define FBT_ARCH FBT_ARCH_32
#endif


#if FBT_PLATFORM == FBT_PLATFORM_WIN32
# if defined(__MINGW32__) || \
     defined(__CYGWIN__)  || \
     (defined (_MSC_VER) && _MSC_VER < 1300)
#  define FBT_INLINE inline
# else
#  define FBT_INLINE __forceinline
# endif
#else
#  define FBT_INLINE    inline
#endif

// Integer types
typedef long            FBTlong;
typedef unsigned long   FBTulong;
typedef int             FBTint32;
typedef unsigned int    FBTuint32;
typedef short           FBTint16;
typedef unsigned short  FBTuint16;
typedef signed char     FBTint8;
typedef unsigned char   FBTuint8;
typedef unsigned char   FBTubyte;
typedef signed char     FBTbyte;
typedef bool            FBTint1;


#if FBT_COMPILER == FBT_COMPILER_GNU
typedef long long          FBTint64;
typedef unsigned long long FBTuint64;
#else
typedef __int64          FBTint64;
typedef unsigned __int64 FBTuint64;
#endif

// Arch dependent types

#if FBT_ARCH == FBT_ARCH_64
typedef FBTuint64   FBTuintPtr;
typedef FBTint64    FBTintPtr;
#else
typedef FBTuint32   FBTuintPtr;
typedef FBTint32    FBTintPtr;
#endif

typedef FBTuintPtr  FBTsize;


// Type for arrays & tables (Always unsigned & 32bit)
typedef FBTuint32       FBTsizeType;
const   FBTuint32       FBT_NPOS = (FBTuint32) - 1;
typedef FBTuint32       FBThash;
typedef FBTuint16       FBTtype;


FBT_ASSERTCOMP(FBTsizeType_NPOS_VALIDATE, FBT_NPOS == ((FBTuint32) - 1));

#define _FBT_CACHE_LIMIT 999

template <typename T> FBT_INLINE void    fbtSwap(T& a, T& b)                              { T t(a); a = b; b = t; }
template <typename T> FBT_INLINE T       fbtMax(const T& a, const T& b)                   { return a < b ? b : a; }
template <typename T> FBT_INLINE T       fbtMin(const T& a, const T& b)                   { return a < b ? a : b; }
template <typename T> FBT_INLINE T       fbtClamp(const T& v, const T& a, const T& b)     { return v < a ? a : v > b ? b : v; }


#define fbtMalloc(size)             ::malloc(size)
#define fbtCalloc(size, len)        ::calloc(size, len)
#define fbtFree(ptr)                ::free(ptr)
#define fbtMemset                   ::memset
#define fbtMemcpy                   ::memcpy
#define fbtMemmove                  ::memmove
#define fbtMemcmp                   ::memcmp



class fbtDebugger
{
public:
	typedef void (*ReportHook) (FBTuintPtr client, const char* buffer);

	struct Reporter
	{
		FBTuintPtr m_client;
		ReportHook m_hook;
	};

	static bool isDebugger(void);
	static void reportIDE(const char* src, long line, const char* msg, ...);
	static void errorIDE(const char* src, long line, const char* msg, ...);
	static void report(const char* msg, ...);
	static void breakProcess(void);

	static void setReportHook(Reporter& hook);

private:
	static Reporter m_report;

};

#define fbtPrintf fbtDebugger::report
#define fbtTRACE  fbtDebugger::reportIDE
#define fbtERROR  fbtDebugger::errorIDE

typedef enum fbtEndian
{
	FBT_ENDIAN_IS_BIG       = 0,
	FBT_ENDIAN_IS_LITTLE    = 1,
	FBT_ENDIAN_NATIVE,
} fbtEndian;

typedef union fbtEndianTest
{
	FBTbyte  bo[4];
	FBTint32 test;
} fbtEndianTest;



FBT_INLINE fbtEndian fbtGetEndian(void)
{
	fbtEndianTest e;
	e.test = FBT_ENDIAN_IS_LITTLE;
	return static_cast<fbtEndian>(e.bo[0]);
}


FBT_INLINE bool fbtIsEndian(const fbtEndian& endian)
{
	fbtEndianTest e;
	e.test = endian;
	return static_cast<fbtEndian>(e.bo[0]) == endian;
}

FBT_INLINE FBTuint16 fbtSwap16(FBTuint16 in)
{
	return static_cast<FBTuint16>(((in & 0xFF00) >> 8) | ((in & 0x00FF) << 8));
}

FBT_INLINE FBTuint32 fbtSwap32(const FBTuint32& in)
{
	return (((in & 0xFF000000) >> 24) | ((in & 0x00FF0000) >> 8) | ((in & 0x0000FF00) << 8)  | ((in & 0x000000FF) << 24));
}

FBT_INLINE FBTint16 fbtSwap16(FBTint16 in)
{
	return fbtSwap16(static_cast<FBTuint16>(in));
}

FBT_INLINE FBTint32 fbtSwap32(const FBTint32& in)
{
	return fbtSwap32(static_cast<FBTuint32>(in));
}


FBT_INLINE FBTuint64 fbtSwap64(const FBTuint64& in)
{
	FBTuint64 r = 0;
	const FBTubyte* src = reinterpret_cast<const FBTubyte*>(&in);
	FBTubyte* dst = reinterpret_cast<FBTubyte*>(&r);

	dst[0] = src[7];
	dst[1] = src[6];
	dst[2] = src[5];
	dst[3] = src[4];
	dst[4] = src[3];
	dst[5] = src[2];
	dst[6] = src[1];
	dst[7] = src[0];
	return r;
}


FBT_INLINE void fbtSwap16(FBTuint16* sp, FBTsize len)
{
	FBTsizeType i;
	for (i = 0; i < len; ++i)
	{
		*sp = fbtSwap16(*sp);
		++sp;	
	}	
}

FBT_INLINE void fbtSwap32(FBTuint32* ip, FBTsize len)
{	 
	FBTsizeType i;
	for (i = 0; i < len; ++i)
	{
		*ip = fbtSwap32(*ip);
		++ip;
	}
}


FBT_INLINE void fbtSwap64(FBTuint64* dp, FBTsize len)
{	 
	FBTsizeType i;
	for (i = 0; i < len; ++i)
	{
		*dp = fbtSwap64(*dp);
		++dp;
	}
}


FBT_INLINE FBTint64 fbtSwap64(const FBTint64& in)
{
	return fbtSwap64(static_cast<FBTuint64>(in));
}



enum fbtPointerLen
{
	FBT_VOID  = sizeof(void*),
	FBT_VOID4 = FBT_VOID == 4,
	FBT_VOID8 = FBT_VOID == 8,
};


#if FBT_ARCH == FBT_ARCH_64
FBT_ASSERTCOMP(FBT_VOID8_ASSERT, FBT_VOID8);
#else
FBT_ASSERTCOMP(FBT_VOID4_ASSERT, FBT_VOID4);
#endif




class fbtList
{
public:

	struct Link
	{
		Link*   next;
		Link*   prev;
	};


	Link*   first;
	Link*   last;

public:

	fbtList() : first(0), last(0) {}
	~fbtList() { clear(); }

	void clear(void) { first = last = 0; }

	void push_back(void* v)
	{
		Link* link = ((Link*)v);
		if (!link)
			return;

		link->prev = last;
		if (last)
			last->next = link;

		if (!first)
			first = link;

		last = link;
	}
};





template <typename T>
class fbtArrayIterator
{
public:
	typedef typename T::Pointer             Iterator;
	typedef typename T::ReferenceType       ValueType;
	typedef typename T::ConstReferenceType  ConstValueType;

protected:

	mutable Iterator        m_iterator;
	mutable FBTsizeType     m_cur;
	mutable FBTsizeType     m_capacity;

public:


	fbtArrayIterator() : m_iterator(0), m_cur(0), m_capacity(0) {}
	fbtArrayIterator(Iterator begin, FBTsizeType size) : m_iterator(begin), m_cur(0), m_capacity(size) {}
	fbtArrayIterator(T& v) : m_iterator(v.ptr()), m_cur(0), m_capacity(v.size()) { }
    fbtArrayIterator(T& v,FBTsizeType cur) : m_iterator(v.ptr()), m_cur(cur), m_capacity(v.size()) { }


	~fbtArrayIterator() {}

    FBT_INLINE bool           hasMoreElements(void) const { return m_iterator && m_cur < m_capacity; }
	FBT_INLINE ValueType      getNext(void)               { FBT_ASSERT(hasMoreElements()); return m_iterator[m_cur++];  }
	FBT_INLINE ConstValueType getNext(void) const         { FBT_ASSERT(hasMoreElements()); return m_iterator[m_cur++];  }
	FBT_INLINE void           next(void) const            { FBT_ASSERT(hasMoreElements()); ++m_cur; }
	FBT_INLINE ValueType      peekNext(void)              { FBT_ASSERT(hasMoreElements()); return m_iterator[m_cur]; }
	FBT_INLINE ConstValueType peekNext(void) const        { FBT_ASSERT(hasMoreElements()); return m_iterator[m_cur]; }

    FBT_INLINE bool operator==(const fbtArrayIterator& rhs) const {return (m_iterator==rhs.m_iterator && m_cur==rhs.m_cur && m_capacity==rhs.m_capacity);}  // default should work
    FBT_INLINE static fbtArrayIterator Find(const fbtArrayIterator& start,const fbtArrayIterator& finish,ConstValueType v) {
        FBT_ASSERT(start.m_iterator==finish.m_iterator);
        FBT_ASSERT(start.m_capacity==finish.m_capacity);
        FBT_ASSERT(start.m_cur<finish.m_cur);
        for (FBTsizeType i = start.m_cur; i < finish.m_cur; i++)   {
            if (start.m_iterator[i] == v) return fbtArrayIterator(start,i);
        }
        return fbtArrayIterator(start,finish.m_cur);
    }

protected:
    fbtArrayIterator(const fbtArrayIterator& rhs,FBTsizeType cur) : m_iterator(rhs.m_iterator), m_cur(cur), m_capacity(rhs.m_capacity) { }


};

template <typename T>
class fbtArray
{
public:
	typedef T*           Pointer;
	typedef const T*     ConstPointer;

	typedef T            ValueType;
	typedef const T      ConstValueType;

	typedef T&           ReferenceType;
	typedef const T&     ConstReferenceType;

	typedef fbtArrayIterator<fbtArray<T> >       Iterator;
	typedef const fbtArrayIterator<fbtArray<T> > ConstIterator;

    //typedef Iterator        iterator;
    //typedef ConstIterator   const_Iterator;


public:
	fbtArray() : m_size(0), m_capacity(0), m_data(0), m_cache(0)  {}

    fbtArray(FBTsizeType size) : m_size(0), m_capacity(0), m_data(0), m_cache(0)  {resize(size);}

    fbtArray(FBTsizeType size,const T& fill) : m_size(0), m_capacity(0), m_data(0), m_cache(0)  {resize(size,fill);}

	fbtArray(const fbtArray<T>& o)
		: m_size(o.size()), m_capacity(0), m_data(0), m_cache(0)
	{
		reserve(m_size);
		copy(m_data, o.m_data, m_size);
	}

	~fbtArray() { clear(); }

	void clear(bool useCache = false)
	{
		if (!useCache)
		{
			if (m_data)
				delete []m_data;
			m_data = 0;
			m_capacity = 0;
			m_size = 0;
			m_cache = 0;
		}
		else
		{
			++m_cache;
			if (m_cache > _FBT_CACHE_LIMIT )
				clear(false);
			else m_size = 0;
		}
	}

	FBTsizeType find(const T& v)
	{
		for (FBTsizeType i = 0; i < m_size; i++)
		{
			if (m_data[i] == v)
				return i;
		}
		return FBT_NPOS;
	}

    FBT_INLINE static Iterator Find(const Iterator& start,const Iterator& finish,const ConstValueType& v) {return Iterator::Find(start,finish,v);}

	FBT_INLINE void push_back(const T& v)
	{
        if (m_size == m_capacity)   {
            const T v_val = v;  // Hehe, v can be a reference to old m_data here!
            reserve(m_size == 0 ? 8 : (m_size * 2));
            m_data[m_size] = v_val;
        }
        else m_data[m_size] = v;
		m_size++;
	}

	FBT_INLINE void pop_back(void)
	{
		m_size--;
		m_data[m_size].~T();
	}

	void erase(const T& v)
	{
		erase(find(v));
	}

	void erase(FBTsizeType pos)
	{
		if (m_size > 0)
		{
			if (pos != FBT_NPOS)
			{
				swap(pos, m_size - 1);
				m_size--;
				m_data[m_size].~T();
			}
		}
	}

	void resize(FBTsizeType nr)
	{
		if (nr < m_size)
		{
			for (FBTsizeType i = m_size; i < nr; i++)
				m_data[i].~T();
		}
		else
		{
			if (nr > m_size)
				reserve(nr);
		}
		m_size = nr;
	}

	void resize(FBTsizeType nr, const T& fill)
	{
		if (nr < m_size)
		{
			for (FBTsizeType i = m_size; i < nr; i++)
				m_data[i].~T();
		}
		else
		{
			if (nr > m_size)
				reserve(nr);
			for (FBTsizeType i = m_size; i < nr; i++)
				m_data[i] = fill;

		}
		m_size = nr;
	}

	void reserve(FBTsizeType nr)
	{

		if (m_capacity < nr)
		{
			T* p = new T[nr];
			if (m_data != 0)
			{
				copy(p, m_data, m_size);
				delete []m_data;
			}
			m_data = p;
			m_capacity = nr;
		}
	}

	void sort(bool (*cmp)(const T& a, const T& b))
	{
		if (m_size > 1 && cmp)
			_sort(cmp, 0, m_size - 1);
	}

	FBT_INLINE T& operator[](FBTsizeType idx)               { FBT_ASSERT(idx >= 0 && idx < m_capacity); return m_data[idx]; }
	FBT_INLINE const T& operator[](FBTsizeType idx) const   { FBT_ASSERT(idx >= 0 && idx < m_capacity); return m_data[idx]; }
	FBT_INLINE T& at(FBTsizeType idx)                       { FBT_ASSERT(idx >= 0 && idx < m_capacity); return m_data[idx]; }
	FBT_INLINE const T& at(FBTsizeType idx) const           { FBT_ASSERT(idx >= 0 && idx < m_capacity); return m_data[idx]; }
	FBT_INLINE T& front(void)                               { FBT_ASSERT(m_size > 0); return m_data[0]; }
	FBT_INLINE T& back(void)                                { FBT_ASSERT(m_size > 0); return m_data[m_size-1]; }

	FBT_INLINE ConstPointer ptr(void) const             { return m_data; }
	FBT_INLINE Pointer      ptr(void)                   { return m_data; }
	FBT_INLINE bool         valid(void) const           { return m_data != 0;}

	FBT_INLINE FBTsizeType capacity(void) const         { return m_capacity; }
	FBT_INLINE FBTsizeType size(void) const             { return m_size; }
	FBT_INLINE bool empty(void) const                   { return m_size == 0;}

    FBT_INLINE Iterator       iterator(void)       { return m_data && m_size > 0 ? Iterator(m_data, m_size) : Iterator(); }
    FBT_INLINE ConstIterator  iterator(void) const { return m_data && m_size > 0 ? ConstIterator(m_data, m_size) : ConstIterator(); }

    FBT_INLINE Iterator       begin(void)       { return m_data && m_size > 0 ? Iterator(m_data, m_size) : Iterator(); }
    FBT_INLINE ConstIterator  begin(void) const { return m_data && m_size > 0 ? ConstIterator(m_data, m_size) : ConstIterator(); }
    FBT_INLINE Iterator       end(void)       { return m_data && m_size > 0 ? Iterator(*this, m_size) : Iterator(); }
    FBT_INLINE ConstIterator  end(void) const { return m_data && m_size > 0 ? ConstIterator(*this, m_size) : ConstIterator(); }


	fbtArray<T> &operator= (const fbtArray<T> &rhs)
	{
		if (this != &rhs)
		{
			clear();
			FBTsizeType os = rhs.size();
			if (os > 0)
			{
				resize(os);
				copy(m_data, rhs.m_data, os);
			}
		}

		return *this;
	}

	FBT_INLINE void copy(Pointer dst, ConstPointer src, FBTsizeType size)
	{
		FBT_ASSERT(size <= m_size);
		for (FBTsizeType i = 0; i < size; i++) dst[i] = src[i];
	}

	FBT_INLINE bool equal(const fbtArray<T> &rhs)
	{
		if (rhs.size() != size()) return false;
		if (empty()) return true;
		return fbtMemcmp(m_data, rhs.m_data, sizeof(T)*m_size) == 0;
	}

    // Not too sure about this
    FBT_INLINE void swap(fbtArray<T> &rhs)  {
        FBTsizeType rhs_size = rhs.m_size; rhs.m_size = m_size; m_size = rhs_size;
        FBTsizeType rhs_cap = rhs.m_capacity; rhs.m_capacity = m_capacity; m_capacity = rhs_cap;
        int rhs_cache = rhs.m_cache; rhs.m_cache = m_cache; m_cache = rhs_cache;    // Waht's m_cache  ?
        Pointer rhs_data = rhs.m_data; rhs.m_data = m_data; m_data = rhs_data;
    }

protected:

	void _sort(bool (*cmp)(const T& a, const T& b), int lo, int hi)
	{
		// btAlignedObjectArray.h

		int i = lo, j = hi;
		T x = m_data[(lo+hi)/2];

		//  partition
		do
		{
			while (cmp(m_data[i], x))
				i++;
			while (cmp(x, m_data[j]))
				j--;
			if (i <= j)
			{
				swap(i, j);
				i++; j--;
			}
		}
		while (i <= j);

		//  recursion
		if (lo < j)
			_sort( cmp, lo, j);
		if (i < hi)
			_sort( cmp, i, hi);
	}


	void swap(FBTsizeType a, FBTsizeType b)
	{
		ValueType t = m_data[a];
		m_data[a] = m_data[b];
		m_data[b] = t;
	}

	FBTsizeType     m_size;
	FBTsizeType     m_capacity;
	Pointer         m_data;
	int             m_cache;
};



template <typename T>
class fbtHashTableIterator
{
public:

	typedef typename T::Pointer        Iterator;
	typedef typename T::Entry&          Pair;
	typedef typename T::ConstEntry&     ConstPair;

	typedef typename T::ReferenceKeyType         KeyType;
	typedef typename T::ReferenceValueType       ValueType;
	typedef typename T::ConstReferenceKeyType    ConstKeyType;
	typedef typename T::ConstReferenceValueType  ConstValueType;

protected:

	mutable Iterator m_iterator;
	mutable FBTsizeType   m_cur;
	const FBTsizeType     m_capacity;


public:
	fbtHashTableIterator() : m_iterator(0), m_cur(0), m_capacity(0)  {}
	fbtHashTableIterator(Iterator begin, FBTsizeType size) : m_iterator(begin), m_cur(0), m_capacity(size) { }
	fbtHashTableIterator(T& v) : m_iterator(v.ptr()), m_cur(0), m_capacity(v.size()) {}

	~fbtHashTableIterator() {}

	FBT_INLINE bool      hasMoreElements(void) const  { return (m_iterator && m_cur < m_capacity); }
	FBT_INLINE Pair      getNext(void)                { FBT_ASSERT(hasMoreElements()); return m_iterator[m_cur++];}
	FBT_INLINE ConstPair getNext(void) const          { FBT_ASSERT(hasMoreElements()); return m_iterator[m_cur++];}
	FBT_INLINE void      next(void) const             { FBT_ASSERT(hasMoreElements()); ++m_cur; }


	FBT_INLINE Pair      peekNext(void)               { FBT_ASSERT(hasMoreElements()); return m_iterator[m_cur]; }
	FBT_INLINE KeyType   peekNextKey(void)            { FBT_ASSERT(hasMoreElements()); return m_iterator[m_cur].first;}
	FBT_INLINE ValueType peekNextValue(void)          { FBT_ASSERT(hasMoreElements()); return m_iterator[m_cur].second; }

	FBT_INLINE ConstPair      peekNext(void)  const     { FBT_ASSERT(hasMoreElements()); return m_iterator[m_cur]; }
	FBT_INLINE ConstKeyType   peekNextKey(void) const   { FBT_ASSERT(hasMoreElements()); return m_iterator[m_cur].first;}
	FBT_INLINE ConstValueType peekNextValue(void) const { FBT_ASSERT(hasMoreElements()); return m_iterator[m_cur].second; }
};



// magic numbers from http://www.isthe.com/chongo/tech/comp/fnv/
#define _FBT_INITIAL_FNV  0x9E3779B1
#define _FBT_INITIAL_FNV2 0x9E3779B9
#define _FBT_MULTIPLE_FNV 0x1000193
#define _FBT_TWHASH(key)         \
        key += ~(key << 15);    \
        key ^=  (key >> 10);    \
        key +=  (key << 3);     \
        key ^=  (key >> 6);     \
        key += ~(key << 11);    \
        key ^=  (key >> 16);


class fbtCharHashKey
{
protected:
	char* m_key;
	mutable FBThash m_hash;

public:
	fbtCharHashKey() : m_key(0), m_hash(FBT_NPOS) {}
	fbtCharHashKey(char* k) : m_key(k), m_hash(FBT_NPOS) {hash();}
	fbtCharHashKey(const char* k) : m_key(const_cast<char*>(k)), m_hash(FBT_NPOS) {}
	fbtCharHashKey(const fbtCharHashKey& k) : m_key(k.m_key), m_hash(k.m_hash) { if (m_hash == FBT_NPOS) hash(); }


	FBThash hash(void) const
	{
		if (!m_key) return FBT_NPOS;
		if (m_hash != FBT_NPOS) return m_hash;

		// Fowler / Noll / Vo (FNV) Hash
		m_hash = (FBThash)_FBT_INITIAL_FNV;
		for (int i = 0; m_key[i]; i++)
		{
			m_hash = m_hash ^ (m_key[i]);   // xor  the low 8 bits
			m_hash = m_hash * _FBT_MULTIPLE_FNV;  // multiply by the magic number
		}
		return m_hash;
	}

	FBT_INLINE bool operator== (const fbtCharHashKey& v) const    {return hash() == v.hash();}
	FBT_INLINE bool operator!= (const fbtCharHashKey& v) const    {return hash() != v.hash();}
	FBT_INLINE bool operator== (const FBThash& v) const           {return hash() == v;}
	FBT_INLINE bool operator!= (const FBThash& v) const           {return hash() != v;}
};

class fbtIntHashKey
{
protected:
	FBTint32 m_key;
public:
	fbtIntHashKey() : m_key(0) {}
	fbtIntHashKey(FBTint32 k) : m_key(k) {}
	fbtIntHashKey(const fbtIntHashKey& k) : m_key(k.m_key) { }

	FBT_INLINE FBThash hash(void) const  { return static_cast<FBThash>(m_key) * _FBT_INITIAL_FNV; }

	FBT_INLINE bool operator== (const fbtIntHashKey& v) const {return hash() == v.hash();}
	FBT_INLINE bool operator!= (const fbtIntHashKey& v) const {return hash() != v.hash();}
	FBT_INLINE bool operator== (const FBThash& v) const       {return hash() == v;}
	FBT_INLINE bool operator!= (const FBThash& v) const       {return hash() != v;}
};



//For handing invalid pointers
class fbtSizeHashKey
{
protected:

	FBTsize m_key;
	mutable FBThash m_hash;

public:
	fbtSizeHashKey()
        : m_key(0), m_hash(FBT_NPOS)
	{
	}


	fbtSizeHashKey(const FBTsize& k)
        :   m_key(k), m_hash(FBT_NPOS)
	{
		hash();
	}


	fbtSizeHashKey(const fbtSizeHashKey& k)
        :   m_key(k.m_key), m_hash(FBT_NPOS)
	{
		hash();
	}


	FBT_INLINE FBThash hash(void) const
	{
		if (m_hash != FBT_NPOS)
			return m_hash;

		m_hash = (FBThash)m_key;
		_FBT_TWHASH(m_hash);
		return m_hash;
	}

	FBT_INLINE bool operator== (const fbtSizeHashKey& v) const  { return hash() == v.hash();}
	FBT_INLINE bool operator!= (const fbtSizeHashKey& v) const  { return hash() != v.hash();}
	FBT_INLINE bool operator== (const FBThash& v) const         { return hash() == v;}
	FBT_INLINE bool operator!= (const FBThash& v) const         { return hash() != v;}
};


template<typename T>
class fbtTHashKey
{
protected:
	T* m_key;
	mutable FBThash m_hash;

public:
	fbtTHashKey() : m_key(0), m_hash(FBT_NPOS) { hash(); }
	fbtTHashKey(T* k) : m_key(k), m_hash(FBT_NPOS) { hash(); }
	fbtTHashKey(const fbtTHashKey& k) : m_key(k.m_key), m_hash(k.m_hash) { if (m_hash == FBT_NPOS) hash(); }

	FBT_INLINE T*          key(void)       {return m_key;}
	FBT_INLINE const T*    key(void) const {return m_key;}


	FBT_INLINE FBThash hash(void) const
	{
		if (m_hash != FBT_NPOS)
			return m_hash;

		// Yields the least collisions.
		m_hash = static_cast<FBThash>(reinterpret_cast<FBTuintPtr>(m_key));
		_FBT_TWHASH(m_hash);
		return m_hash;
	}


	FBT_INLINE bool operator== (const fbtTHashKey& v) const  { return hash() == v.hash();}
	FBT_INLINE bool operator!= (const fbtTHashKey& v) const  { return hash() != v.hash();}
	FBT_INLINE bool operator== (const FBThash& v) const      { return hash() == v;}
	FBT_INLINE bool operator!= (const FBThash& v) const      { return hash() != v;}
};

typedef fbtTHashKey<void> fbtPointerHashKey;


template<typename Key, typename Value>
struct fbtHashEntry
{
	Key    first;
	Value  second;

	fbtHashEntry() {}
	fbtHashEntry(const Key& k, const Value& v) : first(k), second(v) {}

	FBT_INLINE bool operator==(const fbtHashEntry& rhs) const
	{
		return first == rhs.first && second == rhs.second;
	}
};

#define _FBT_UTHASHTABLE_HASH(key)      ((key.hash() & (m_capacity - 1)))
#define _FBT_UTHASHTABLE_HKHASH(key)    ((hk & (m_capacity - 1)))
#define _FBT_UTHASHTABLE_FORCE_POW2     1
#define _FBT_UTHASHTABLE_INIT           32
#define _FBT_UTHASHTABLE_EXPANSE  (m_size * 2)


#define _FBT_UTHASHTABLE_STAT       FBT_HASHTABLE_STAT
#define _FBT_UTHASHTABLE_STAT_ALLOC 0


#if _FBT_UTHASHTABLE_FORCE_POW2 == 1
#define _FBT_UTHASHTABLE_POW2(x) \
    --x; x |= x >> 16; x |= x >> 8; x |= x >> 4; \
    x |= x >> 2; x |= x >> 1; ++x;

#define _FBT_UTHASHTABLE_IS_POW2(x) (x && !((x-1) & x))
#endif


#if _FBT_UTHASHTABLE_STAT == 1
#include <typeinfo>
#endif



template < typename Key, typename Value >
class fbtHashTable
{
public:
	typedef fbtHashEntry<Key, Value>        Entry;
	typedef const fbtHashEntry<Key, Value>  ConstEntry;

	typedef Entry*  EntryArray;
	typedef FBTsizeType* IndexArray;


	typedef Key            KeyType;
	typedef Value          ValueType;

	typedef const Key      ConstKeyType;
	typedef const Value    ConstValueType;

	typedef Value&          ReferenceValueType;
	typedef const Value&    ConstReferenceValueType;

	typedef Key&            ReferenceKeyType;
	typedef const Key&      ConstReferenceKeyType;

	typedef EntryArray Pointer;
	typedef const Entry* ConstPointer;


	typedef fbtHashTableIterator<fbtHashTable<Key, Value> > Iterator;
	typedef const fbtHashTableIterator<fbtHashTable<Key, Value> > ConstIterator;

    //typedef Iterator            iterator;
    //typedef ConstIterator       const_iterator;


public:

	fbtHashTable()
        :    m_size(0), m_capacity(0), m_lastPos(FBT_NPOS), m_lastKey(FBT_NPOS),
		     m_iptr(0), m_nptr(0), m_bptr(0), m_cache(0)
	{
	}

	fbtHashTable(FBTsizeType capacity)
        :    m_size(0), m_capacity(0), m_lastPos(FBT_NPOS), m_lastKey(FBT_NPOS),
		     m_iptr(0), m_nptr(0), m_bptr(0), m_cache(0)
	{
	}

	fbtHashTable(const fbtHashTable& rhs)
        :    m_size(0), m_capacity(0), m_lastPos(FBT_NPOS), m_lastKey(FBT_NPOS),
		     m_iptr(0), m_nptr(0), m_bptr(0), m_cache(0)
	{
		doCopy(rhs);
	}

	~fbtHashTable() { clear(); }

	fbtHashTable<Key, Value> &operator = (const fbtHashTable<Key, Value> &rhs)
	{
		if (this != &rhs)
			doCopy(rhs);
		return *this;
	}

	void clear(bool useCache = false)
	{
		if (!useCache)
		{
			m_size = m_capacity = 0;
			m_lastKey = FBT_NPOS;
			m_lastPos = FBT_NPOS;
			m_cache = 0;

			delete [] m_bptr;
			delete [] m_iptr;
			delete [] m_nptr;
			m_bptr = 0; m_iptr = 0; m_nptr = 0;
		}
		else
		{
			++m_cache;
			if (m_cache > _FBT_CACHE_LIMIT)
				clear(false);
			else
			{
				m_size = 0;
				m_lastKey = FBT_NPOS;
				m_lastPos = FBT_NPOS;


				FBTsizeType i;
				for (i = 0; i < m_capacity; ++i)
				{
					m_iptr[i] = FBT_NPOS;
					m_nptr[i] = FBT_NPOS;
				}
			}
		}

	}
	Value&              at(FBTsizeType i)                    { FBT_ASSERT(m_bptr && i >= 0 && i < m_size); return m_bptr[i].second; }
	Value&              operator [](FBTsizeType i)           { FBT_ASSERT(m_bptr && i >= 0 && i < m_size); return m_bptr[i].second; }
	const Value&        at(FBTsizeType i)const               { FBT_ASSERT(m_bptr && i >= 0 && i < m_size); return m_bptr[i].second; }
	const Value&        operator [](FBTsizeType i) const     { FBT_ASSERT(m_bptr && i >= 0 && i < m_size); return m_bptr[i].second; }
	Key&                keyAt(FBTsizeType i)                 { FBT_ASSERT(m_bptr && i >= 0 && i < m_size); return m_bptr[i].first; }
	const Key&          keyAt(FBTsizeType i)const            { FBT_ASSERT(m_bptr && i >= 0 && i < m_size); return m_bptr[i].first; }

	Value* get(const Key& key) const
	{
		if (!m_bptr || m_size == 0)
			return (Value*)0;


		FBThash hr = key.hash();

		if (m_lastKey != hr)
		{
			FBTsizeType i = find(key);
			if (i == FBT_NPOS) return (Value*)0;


			FBT_ASSERT(i >= 0 && i < m_size);

			m_lastKey = hr;
			m_lastPos = i;
		}

		return &m_bptr[m_lastPos].second;
	}


	Value*         operator [](const Key& key)       { return get(key); }
	const Value*   operator [](const Key& key) const { return get(key); }

	FBTsizeType find(const Key& key) const
	{
		if (m_capacity == 0 || m_capacity == FBT_NPOS || m_size == 0)
			return FBT_NPOS;

		FBTsizeType hk = key.hash();
		if (m_lastPos != FBT_NPOS && m_lastKey == hk)
			return m_lastPos;

		FBThash hr = _FBT_UTHASHTABLE_HKHASH(hk);

		FBT_ASSERT(m_bptr && m_iptr && m_nptr);

		FBTsizeType fh = m_iptr[hr];
		while (fh != FBT_NPOS && (key != m_bptr[fh].first))
			fh = m_nptr[fh];


		if (fh != FBT_NPOS)
		{
			m_lastKey = hk;
			m_lastPos = fh;

			FBT_ASSERT(fh >= 0  && fh < m_size);
		}
		return fh;
	}



	void erase(const Key& key) {remove(key);}

	void remove(const Key& key)
	{
		FBThash hash, lhash;
		FBTsizeType index, pindex, findex;

		findex = find(key);
		if (findex == FBT_NPOS || m_capacity == 0 || m_size == 0)
			return;

		m_lastKey = FBT_NPOS;
		m_lastPos = FBT_NPOS;
		FBT_ASSERT(m_bptr && m_iptr && m_nptr);

		hash = _FBT_UTHASHTABLE_HASH(key);

		index  = m_iptr[hash];
		pindex = FBT_NPOS;
		while (index != findex)
		{
			pindex = index;
			index = m_nptr[index];
		}

		if (pindex != FBT_NPOS)
		{
			FBT_ASSERT(m_nptr[pindex] == findex);
			m_nptr[pindex] = m_nptr[findex];
		}
		else
			m_iptr[hash] = m_nptr[findex];

		FBTsizeType lindex = m_size - 1;
		if (lindex == findex)
		{
			--m_size;
			m_bptr[m_size].~Entry();
			return;
		}

		lhash = _FBT_UTHASHTABLE_HASH(m_bptr[lindex].first);
		index  = m_iptr[lhash];
		pindex = FBT_NPOS;
		while (index != lindex)
		{
			pindex = index;
			index = m_nptr[index];
		}

		if (pindex != FBT_NPOS)
		{
			FBT_ASSERT(m_nptr[pindex] == lindex);
			m_nptr[pindex] = m_nptr[lindex];
		}
		else
			m_iptr[lhash] = m_nptr[lindex];

		m_bptr[findex] = m_bptr[lindex];
		m_nptr[findex] = m_iptr[lhash];
		m_iptr[lhash] = findex;

		--m_size;
		m_bptr[m_size].~Entry();
		return;
	}

	bool insert(const Key& key, const Value& val)
	{
		if (find(key) != FBT_NPOS)
			return false;

		if (m_size == m_capacity)
			reserve(m_size == 0 ? _FBT_UTHASHTABLE_INIT : _FBT_UTHASHTABLE_EXPANSE);

		const FBThash hr = _FBT_UTHASHTABLE_HASH(key);

		FBT_ASSERT(m_bptr && m_iptr && m_nptr);
		m_bptr[m_size] = Entry(key, val);
		m_nptr[m_size] = m_iptr[hr];
		m_iptr[hr] = m_size;
		++m_size;
		return true;
	}

	FBT_INLINE Pointer ptr(void)                { return m_bptr; }
	FBT_INLINE ConstPointer ptr(void) const     { return m_bptr; }
	FBT_INLINE bool valid(void) const           { return m_bptr != 0;}


	FBT_INLINE FBTsizeType size(void) const         { return m_size; }
	FBT_INLINE FBTsizeType capacity(void) const     { return capacity; }
	FBT_INLINE bool empty(void) const               { return m_size == 0; }


    Iterator        iterator(void)       { return m_bptr && m_size > 0 ? Iterator(m_bptr, m_size) : Iterator(); }
    ConstIterator   iterator(void) const { return m_bptr && m_size > 0 ? ConstIterator(m_bptr, m_size) : ConstIterator(); }

    Iterator        begin(void)       { return m_bptr && m_size > 0 ? Iterator(m_bptr, m_size) : Iterator(); }
    ConstIterator   begin(void) const { return m_bptr && m_size > 0 ? ConstIterator(m_bptr, m_size) : ConstIterator(); }


	void reserve(FBTsizeType nr)
	{
		if (m_capacity < nr && nr != FBT_NPOS)
			rehash(nr);
	}

#if _FBT_UTHASHTABLE_STAT == 1

	void report(void) const
	{
		if (m_capacity == 0 || m_capacity == FBT_NPOS || m_size == 0)
			return;

		FBT_ASSERT(m_bptr && m_iptr && m_nptr);


		FBTsizeType min_col = m_size, max_col = 0;
		FBTsizeType i, tot = 0, avg = 0;
		for (i = 0; i < m_size; ++i)
		{
			Key& key = m_bptr[i].first;

			FBThash hr = _FBT_UTHASHTABLE_HASH(key);

			FBTsizeType nr = 0;

			FBTsizeType fh = m_iptr[hr];
			while (fh != FBT_NPOS && (key != m_bptr[fh].first))
			{
				fh = m_nptr[fh];
				nr++;
			}

			if (nr < min_col)
				min_col = nr;
			if (nr > max_col)
				max_col = nr;

			tot += nr;
			avg += nr ? 1 : 0;
		}

#if _FBT_UTHASHTABLE_FORCE_POW2 == 1
		fbtPrintf("Results using forced power of 2 expansion.\n\n");
#else
		fbtPrintf("Results using unaltered expansion.\n\n");
#endif
		fbtPrintf("\tTotal number of collisions %i for a table of size %i.\n\t\tusing (%s)\n", tot, m_size, typeid(Key).name());
		fbtPrintf("\tThe minimum number of collisions per key: %i\n", min_col);
		fbtPrintf("\tThe maximum number of collisions per key: %i\n", max_col);

		int favr = (int)(100.f * ((float)avg / (float)m_size));
		fbtPrintf("\tThe average number of key collisions: %i\n\n", favr);

		if (tot == 0)
			fbtPrintf("\nCongratulations lookup is 100%% linear!\n\n");
		else if (favr >  35)
			fbtPrintf("\nImprove your hash function!\n\n");
	}
#endif



private:

	void doCopy(const fbtHashTable<Key, Value> &rhs)
	{
		if (rhs.valid() && !rhs.empty())
		{
			reserve(rhs.m_capacity);

			FBTsizeType i, b;
			m_size     = rhs.m_size;
			m_capacity = rhs.m_capacity;

			b = m_size > 0 ? m_size - 1 : 0;
			for (i = b; i < m_capacity; ++i) m_nptr[i] = m_iptr[i] = FBT_NPOS;

			for (i = 0; i < m_size; ++i)
			{
				m_bptr[i] = rhs.m_bptr[i];
				m_iptr[i] = rhs.m_iptr[i];
				m_nptr[i] = rhs.m_nptr[i];
			}
		}

	}

	template<typename ArrayType>
	void reserveType(ArrayType** old, FBTsizeType nr, bool cpy = false)
	{
		FBTsizeType i;
		ArrayType* nar = new ArrayType[nr];
		if ((*old) != 0)
		{
			if (cpy)
			{
				const ArrayType* oar = (*old);
				for (i = 0; i < m_size; i++) nar[i] = oar[i];
			}
			delete [](*old);
		}
		(*old) = nar;
	}


	void rehash(FBTsizeType nr)
	{
#if _FBT_UTHASHTABLE_FORCE_POW2

		if (!_FBT_UTHASHTABLE_IS_POW2(nr))
		{
			_FBT_UTHASHTABLE_POW2(nr);
		}

#if _FBT_UTHASHTABLE_STAT_ALLOC == 1
		fbtPrintf("Expanding tables: %i\n", nr);
#endif
		FBT_ASSERT(_FBT_UTHASHTABLE_IS_POW2(nr));


#else

#if _FBT_UTHASHTABLE_STAT_ALLOC == 1
		fbtPrintf("Expanding tables: %i\n", nr);
#endif

#endif

		reserveType<Entry>(&m_bptr, nr, true);
		reserveType<FBTsizeType>(&m_iptr, nr);
		reserveType<FBTsizeType>(&m_nptr, nr);

		m_capacity = nr;
		FBT_ASSERT(m_bptr && m_iptr && m_nptr);


		FBTsizeType i, h;
		for (i = 0; i < m_capacity; ++i) { m_iptr[i] = m_nptr[i] = FBT_NPOS; }
		for (i = 0; i < m_size; i++)     { h = _FBT_UTHASHTABLE_HASH(m_bptr[i].first); m_nptr[i] = m_iptr[h]; m_iptr[h] = i;}
	}



	FBTsizeType m_size, m_capacity;
	mutable FBTsizeType m_lastPos;
	mutable FBTsizeType m_lastKey;

	IndexArray m_iptr;
	IndexArray m_nptr;
	EntryArray m_bptr;
	FBTsizeType m_cache;
};




#define fbtCharNEq(a, b, n)  ((a && b) && !strncmp(a, b, n))
#define fbtCharEq(a, b)      ((a && b) && (*a == *b) && !strcmp(a, b))
#define fbtCharEqL(a, b, l)  ((a && b) && (*a == *b) && !memcmp(a, b, l))

FBT_INLINE int fbtStrLen(const char* cp)
{
	int i = 0;
    while (cp[i]) ++i;//cp[i++];
	return i;
}


// For operations on a fixed size character array
template <const FBTuint16 L>
class fbtFixedString
{
public:
	typedef char Pointer[L+1];


public:

	fbtFixedString()
		: m_size(0), m_hash(FBT_NPOS)
	{
		m_buffer[m_size] = 0;
	}

	fbtFixedString(const fbtFixedString& rhs)
		:   m_size(0), m_hash(FBT_NPOS)
	{
		if (rhs.size())
		{
			FBTuint16 i, os = rhs.size();
			const char* cp = rhs.c_str();

			for (i = 0; i < L && i < os; ++i, ++m_size) m_buffer[i] = cp[i];
		}
		m_buffer[m_size] = 0;
	}


	fbtFixedString(const char* rhs)
		:   m_size(0), m_hash(FBT_NPOS)
	{
		if (rhs)
		{
			FBTuint16 i;
			for (i = 0; i < L && rhs[i]; ++i, ++m_size) m_buffer[i] = rhs[i];
		}
		m_buffer[m_size] = 0;
	}



	FBT_INLINE void push_back(char ch)
	{
		if (m_size >= L)
			return;
		m_buffer[m_size++] = ch;
		m_buffer[m_size] = 0;
	}

	void append(const char* str)
	{
		int len = fbtStrLen(str);
		int a = 0;
		while (a < len)
			push_back(str[a++]);
	}


	void append(const fbtFixedString& str)
	{
		int len = str.m_size;
		int a = 0;
		while (a < len)
			push_back(str.m_buffer[a++]);
	}


	fbtFixedString operator +(const fbtFixedString& rhs)
	{
		fbtFixedString lhs = *this;
		lhs.append(rhs);
		return lhs;
	}


	fbtFixedString operator +=(const fbtFixedString& rhs)
	{
		append(rhs);
		return *this;
	}

	fbtFixedString operator +(const char* str)
	{
		fbtFixedString lhs = *this;
		lhs.append(str);
		return lhs;
	}

	fbtFixedString operator +=(const char* str)
	{
		append(str);
		return *this;
	}


	void split(fbtArray<fbtFixedString<L> >& dest, char c, char e = '\0') const
	{
		FBTuint16 i, p = 0, t;
		for (i = 0; i < L && i < m_size; ++i)
		{
			if (m_buffer[i] == c || m_buffer[i] == e)
			{
				fbtFixedString<L> cpy;
				for (t = p; t < i; ++t) cpy.push_back(m_buffer[t]);
				dest.push_back(cpy);
				p = i + 1;
			}
		}

		if (p != i)
		{
			fbtFixedString<L> cpy;
			for (t = p; t < i; ++t) cpy.push_back(m_buffer[t]);
			dest.push_back(cpy);
		}
	}


	void resize(FBTuint16 ns)
	{
		if (ns <= L)
		{
			if (ns < m_size)
				for (FBTuint16 i = ns; i < m_size; i++) m_buffer[i] = 0;
			else
				for (FBTuint16 i = m_size; i < ns; i++) m_buffer[i] = 0;
			m_size = ns;
			m_buffer[m_size] = 0;
		}
	}



	template<const FBTuint16 OL>
	fbtFixedString<L>& operator = (const fbtFixedString<OL>& o)
	{
		if (o.m_size > 0)
		{
			if (m_hash == FBT_NPOS || m_hash != o.m_hash)
			{
				FBTuint16 i;
				m_size = 0;
				m_hash = o.m_hash;
				for (i = 0; (i < L && i < OL) && i < o.m_size; ++i, ++m_size) m_buffer[i] = o.m_buffer[i];
				m_buffer[m_size] = 0;
			}
		}
		return *this;
	}

	FBT_INLINE const char* c_str(void) const                 { return m_buffer; }
	FBT_INLINE char* ptr(void)                               { return m_buffer; }
	FBT_INLINE const char* ptr(void) const                   { return m_buffer; }
    FBT_INLINE char operator [](FBTuint16 i) const           { FBT_ASSERT(i < L); return m_buffer[i]; }
    FBT_INLINE char at(FBTuint16 i) const                    { FBT_ASSERT(i < L); return m_buffer[i]; }
	FBT_INLINE void clear(void)                              { m_buffer[0] = 0; m_size = 0; }
	FBT_INLINE int empty(void) const                         { return m_size == 0; }
	FBT_INLINE int size(void) const                          { return m_size; }
	FBT_INLINE int capacity(void) const                      { return L; }



	FBTsize hash(void) const
	{
		if (m_hash != FBT_NPOS)
			return m_hash;

		if (m_size == 0/* || !m_buffer*/)	// Warning: address of array 'this->m_buffer' will always evaluate to 'true'
			return FBT_NPOS;
		fbtCharHashKey chk(m_buffer);
		m_hash = chk.hash();
		return m_hash;
	}

	FBT_INLINE bool operator == (const fbtFixedString& str) const { return hash() == str.hash(); }
	FBT_INLINE bool operator != (const fbtFixedString& str) const { return !(this->operator ==(str));}

protected:

	Pointer             m_buffer;
	FBTuint16           m_size;
	mutable FBThash     m_hash;
};


// For operations with variable size character array
class fbtString : public fbtArray<char> {
typedef fbtArray<char> base;

public:
FBT_INLINE fbtString(int reservedChars=-1) : base() {if (reservedChars>0) base::reserve(reservedChars+1);base::resize(1);operator[](0)='\0';}
FBT_INLINE fbtString(const fbtString& other) : base() {base::resize(1);operator[](0)='\0';*this = other;}
FBT_INLINE fbtString(const char* str) : base() {
    base::resize(1);operator[](0)='\0';
    *this = str;
}
FBT_INLINE int size() const {
    if (base::size()<1) return 0;
    return base::size()-1;
}
FBT_INLINE int length() const {
    return size();
}
FBT_INLINE bool empty() const {
    return size()==0;
}

FBT_INLINE const char* c_str() const {
    return (internalSize()>0 ? &operator[](0) : NULL);
}
FBT_INLINE int compare(const fbtString& other) const  {
    return (empty() ? -1 : other.empty() ? 1 : strcmp(c_str(),other.c_str()));
}
FBT_INLINE bool operator==(const fbtString& other) const {
    if (internalSize()!=other.internalSize()) return false;
    //for (int i=0,sz=internalSize();i<sz;i++) {if (operator[](i)!=other[i]) return false;}
    //return true;
    return (compare(other)==0);
}
FBT_INLINE bool operator!=(const fbtString& other) const {
    return !operator==(other);
}
FBT_INLINE bool operator<(const fbtString& other) const  {
    return compare(other)<0;
}

FBT_INLINE const fbtString& operator=(const char* other) {
    //printf("operator=(const char* other) START: \"%s\" \"%s\"",this->c_str(),other);
    if (!other) return *this;
    const int len = strlen(other);
    base::resize(len+1);
    for (int i=0;i<len;i++) operator[](i) = other[i];   // strcpy ?
    operator[](len) = '\0';
    //printf("operator=(const char* other) END: \"%s\"\n",this->c_str());
    return *this;
}
FBT_INLINE const fbtString& operator=(const fbtString& other) {
    //printf("operator=(const fbtString& other) START: \"%s\" \"%s\"",this->c_str(),other.c_str());
    //base::operator=(other); // Warning: fbtArray has NO DEFAULT ASSIGNMENT ATM
    // -----------------------
    if (other.size()==0) *this="";
    else operator=(other.c_str());
    // -----------------------
    //printf("operator=(const fbtString& other) END: \"%s\"\n",this->c_str());
    return *this;
}

FBT_INLINE const fbtString& operator+=(const fbtString& other) {
    const int curSize = size();
    if (curSize==0) return operator=(other);
    const int len = other.size();
    base::resize(curSize + len+1);
    for (int i=curSize;i<curSize+len;i++) operator[](i) = other[i-curSize];
    operator[](curSize+len) = '\0';
    return *this;

}
FBT_INLINE const fbtString& operator+=(const char* other) {
    if (!other) return *this;
    const int curSize = size();
    if (curSize==0) return operator=(other);
    const int len = strlen(other);
    base::resize(curSize + len+1);
    for (int i=curSize;i<curSize+len;i++) operator[](i) = other[i-curSize];
    operator[](curSize+len) = '\0';
    return *this;
}
FBT_INLINE const fbtString& operator+=(const char c) {
    const int curSize = internalSize();
    if (curSize==0) {
        resize(2);
        operator[](0) = c;
        operator[](1) = '\0';
    }
    else    {
        base::resize(curSize + 1);
        operator[](curSize-1) = c;
        operator[](curSize) = '\0';
    }
    return *this;
}

static const int npos = -1;


FBT_INLINE int find(const char c,int beg = 0) const  {
    for (int i=beg,sz = size();i<sz;i++)    {
        if (operator[](i) == c) return i;
    }
    return npos;
}
FBT_INLINE int find_first_of(const char c,int beg = 0) const {
    return find(c,beg);
}
FBT_INLINE int find_last_of(const char c,int beg = 0) const  {
    for (int i=size()-1;i>=beg;i--)    {
        if (operator[](i) == c) return i;
    }
    return npos;
}
FBT_INLINE int find(const fbtString& s,int beg = 0) const  {
    int i,j,sz;
    const int ssize = s.size();
    if (ssize == 0 || beg+ssize>=size()) return -1;
    for (i=beg,sz = size()-beg;i<sz;i++)    {
        for (j=0;j<ssize;j++)   {
            if (operator[](i+j) != s.operator [](j)) break;
            if (j==ssize-1) return i;
        }
    }
    return npos;
}
FBT_INLINE int find_first_of(const fbtString& s,int beg = 0) const {
    return find(s,beg);
}
// not tested:
FBT_INLINE int find_last_of(const fbtString& s,int beg = 0) const  {
    int i,j;
    const int ssize = s.size();
    if (ssize == 0 || beg+ssize>=size()) return -1;
    for (i=size()-ssize-1;i>=beg;i--)    {
        for (j=0;j<ssize;j++)   {
            if (operator[](i+j) != s.operator [](j)) break;
            if (j==ssize-1) return i;
        }
    }
    return npos;
}

FBT_INLINE const fbtString substr(int beg,int cnt=-1) const {
    const int sz = size();
    if (beg>=sz) return fbtString("");
    if (cnt==-1) cnt = sz - beg;
    fbtString rv;rv.resize(cnt+1);
    for (int i=0;i<cnt;i++) {
        rv.operator [](i) = this->operator [](beg+i);
    }
    rv.operator [](cnt) = '\0';
    return rv;
}

protected:

private:
FBT_INLINE int internalSize() const {
    return base::size();
}
FBT_INLINE void reserve(int i) {
    return base::reserve(i);
}
FBT_INLINE void resize(int i) {base::resize(i);}
FBT_INLINE void clear() {base::clear();}
FBT_INLINE int findLinearSearch(const char c) {
    for (int i=0,sz=size();i<sz;i++)    {
        if (c == operator[](i)) return i;
    }
    return npos;
}
FBT_INLINE void push_back(const char c) {
    return base::push_back(c);
}

friend void fbtStringNonStdResize(fbtString& s,int size);

//TODO: redefine all the other methods we want to hide here...
};
FBT_INLINE const fbtString operator+(fbtString v1, const fbtString& v2 ) {return v1+=(v2);}



enum FBT_PRIM_TYPE
{
	FBT_PRIM_CHAR,		// 0
	FBT_PRIM_UCHAR,		// 1
	FBT_PRIM_SHORT,		// 2
	FBT_PRIM_USHORT,	// 3
	FBT_PRIM_INT,		// 4
	FBT_PRIM_LONG,		// 5
	FBT_PRIM_ULONG,		// 6
	FBT_PRIM_FLOAT,		// 7
	FBT_PRIM_DOUBLE,	// 8
	FBT_PRIM_VOID,		// 9
	FBT_PRIM_UNKNOWN	// 10
};

FBT_PRIM_TYPE fbtGetPrimType(FBTuint32 typeKey);
FBT_INLINE FBT_PRIM_TYPE fbtGetPrimType(const char* typeName)
{
	return fbtGetPrimType(fbtCharHashKey(typeName).hash());
}
FBT_INLINE bool fbtIsIntType(FBTuint32 typeKey)
{
	FBT_PRIM_TYPE tp = fbtGetPrimType(typeKey);
	return tp < FBT_PRIM_FLOAT;
}
FBT_INLINE bool fbtIsFloatType(FBTuint32 typeKey)
{
	FBT_PRIM_TYPE tp = fbtGetPrimType(typeKey);
	return tp == FBT_PRIM_FLOAT || tp == FBT_PRIM_DOUBLE;
}
FBT_INLINE bool fbtIsNumberType(FBTuint32 typeKey)
{
	FBT_PRIM_TYPE tp = fbtGetPrimType(typeKey);
	return tp != FBT_PRIM_VOID && tp != FBT_PRIM_UNKNOWN;
}



/** @}*/
#endif//_fbtTypes_h_

/** \addtogroup FBT
*  @{
*/

class fbtStream;
class fbtBinTables;

#include <stdio.h> // FILE*, used in the static helper method fbtFile::UTF8_fopen(...)
                   // (unluckily there's no way to forward-declare FILE)

class fbtFile
{
public:

	enum FileMagic
	{
		FM_BIG_ENDIAN       = 'V',
		FM_LITTLE_ENDIAN    = 'v',
		FM_32_BIT           = '_',
		FM_64_BIT           = '-',

		// place custom rules here, note that the
		// full header string must be <= 12 char's
	};


	enum FileStatus
	{
		FS_LINK_FAILED = -7,
		FS_INV_INSERT,
		FS_BAD_ALLOC,
		FS_INV_READ,
		FS_INV_LENGTH,
		FS_INV_HEADER_STR,
		FS_FAILED,
		FS_OK,
	};


	enum ParseMode
	{
		PM_UNCOMPRESSED,
		PM_COMPRESSED,
		PM_READTOMEMORY,

	};

	enum FileHeader
	{
		FH_ENDIAN_SWAP  = (1 << 0),
		FH_CHUNK_64     = (1 << 1),
		FH_VAR_BITS     = (1 << 2),
	};


	struct Chunk32
	{
		FBTuint32       m_code;
		FBTuint32       m_len;
		FBTuint32       m_old;
		FBTuint32       m_typeid;
		FBTuint32       m_nr;
	};

	struct Chunk64
	{
		FBTuint32       m_code;
		FBTuint32       m_len;
		FBTuint64       m_old;
		FBTuint32       m_typeid;
		FBTuint32       m_nr;
	};


	struct Chunk
	{
		FBTuint32       m_code;
		FBTuint32       m_len;
		FBTsize         m_old;
		FBTuint32       m_typeid;
		FBTuint32       m_nr;
	};


	struct MemoryChunk
	{
		enum Flag
		{
			BLK_MODIFIED = (1 << 0),
		};

		MemoryChunk* m_next, *m_prev;
		Chunk        m_chunk;
		void*        m_block;
		void*        m_newBlock;


		FBTuint8     m_flag;
		FBTtype      m_newTypeId;
	};

public:


	fbtFile(const char* uid);
	virtual ~fbtFile();

    int parse(const char* path,const char* uncompressedFileDetectorPrefix="BLENDER");
    int parse(const char* path, int mode/* = PM_UNCOMPRESSED*/);
    int parse(const void* memory, FBTsize sizeInBytes, int mode = PM_UNCOMPRESSED, bool suppressHeaderWarning=false);

	/// Saving in non native endianness is not implemented yet.
	int reflect(const char* path, const int mode = PM_UNCOMPRESSED, const fbtEndian& endian = FBT_ENDIAN_NATIVE);


	const fbtFixedString<12>&   getHeader(void)     const {return m_header;}
	const int&                  getVersion(void)    const {return m_fileVersion;}
	const char*                 getPath(void)       const {return m_curFile; }


	fbtBinTables* getMemoryTable(void)  {return m_memory;}
	fbtBinTables* getFileTable(void)    {return m_file;}


	fbtList& getChunks(void) {return m_chunks;}

    virtual void setIgnoreList(FBTuint32 * /*stripList*/) {}

	bool _setuid(const char* uid);

    // General Programming Staic Helper Methods:
    static FILE* UTF8_fopen(const char* filename, const char* mode);    // Used to allow UTF8 chars on Windows
    static unsigned char* FBT_GetFileContent(const char *filePath,unsigned long* pSizeOut=NULL,const char modes[] = "rb");  // Returns a memory buffer (that user must free using: delete[] mybuffname;) with the content of 'filePath'.


protected:

	int parseMagic(const char* cp);

	void writeStruct(fbtStream* stream, FBTtype index, FBTuint32 code, FBTsize len, void* writeData);
	void writeBuffer(fbtStream* stream, FBTsize len, void* writeData);


	virtual int initializeTables(fbtBinTables* tables) = 0;
    virtual int notifyData(void* /*p*/, const Chunk& /*id*/) {return FS_OK;}
    virtual int writeData(fbtStream* /*stream*/) {return FS_OK;}
	
	virtual void*   getFBT(void) = 0;
	virtual FBTsize getFBTlength(void) = 0;


	// lookup name first 7 of 12
	const char* m_uhid;
	const char* m_aluhid; //alternative header string
	fbtFixedString<12> m_header;


	int m_version, m_fileVersion, m_fileHeader;
	char* m_curFile;

	typedef fbtHashTable<fbtSizeHashKey, MemoryChunk*> ChunkMap;
	fbtList     m_chunks;
	ChunkMap    m_map;
	fbtBinTables* m_memory, *m_file;


    virtual bool skip(const FBTuint32& /*id*/) {return false;}
	void* findPtr(const FBTsize& iptr);
	MemoryChunk* findBlock(const FBTsize& iptr);

    static bool FileStartsWith(const char* path,const char* cmp);    // Used to detect if a .blend file is not compressed

private:


	int parseHeader(fbtStream* stream, bool suppressHeaderWarning=false);
	int parseStreamImpl(fbtStream* stream, bool suppressHeaderWarning=false);

	int compileOffsets(void);
	int link(void);
};

/** @}*/
#endif//_fbtFile_h_




class fbtBlend : public fbtFile
{
public:
	fbtBlend();
	virtual ~fbtBlend();

    // Warning: these fields are NOT updated automatically, but must be MANUALLY integrated (to reflect the content of source/blender/makesdna/DNA_ID_enums.h)
    // It should be OK to use "integrated versions" also for older versions (newer fields are not present in their Blender DNA, and this should be OK). [DNA_ID_enums 1/2]
	fbtList m_scene;
	fbtList m_library;
	fbtList m_object;
	fbtList m_mesh;
	fbtList m_curve;	// In Blender>=3.1 it's obsolated and marked 'legacy'. New .blend files should use m_curves instead (see below)
	fbtList m_mball;    // MataBall
	fbtList m_mat;      // Material
	fbtList m_tex;      // Texture
	fbtList m_image;
	fbtList m_latt;     // Lattice
	fbtList m_lamp;
	fbtList m_camera;
	fbtList m_ipo;      // or FCurves
	fbtList m_key;      // ShapeKey
	fbtList m_world;
	fbtList m_screen;
	fbtList m_script;   // Python   (used in older versions)
	fbtList m_vfont;    // VectorFont
	fbtList m_text;
	fbtList m_speaker;
	fbtList m_sound;
	fbtList m_group;    // Group/Collection
	fbtList m_armature;
	fbtList m_action;
	fbtList m_nodetree;
	fbtList m_brush;
	fbtList m_particle; // ParticleSettings
	fbtList m_gpencilLegacy;
	fbtList m_wm;       // WindowManager	
	fbtList m_movieClip;
	fbtList m_mask;
	fbtList m_freeStyleLineStyle;
	fbtList m_palette;
    fbtList m_paintCurve;
	fbtList m_cacheFile;
	fbtList m_workSpace;
	fbtList m_lightProbe;
	fbtList m_hair;		// Removed in Blender 3.1: replaced by m_curves.
	fbtList m_curves;	// Replacement for m_hair (and m_curve) in Blender>=3.1
	fbtList m_pointCloud;
	fbtList m_volume;
	fbtList m_simulation;// GeometryNodeGroups
	fbtList m_gpencil;

	Blender::FileGlobal* m_fg;

	int save(const char* path, const int mode = PM_UNCOMPRESSED);
	
	void setIgnoreList(FBTuint32 *stripList) {m_stripList = stripList;}

protected:
	virtual int notifyData(void* p, const Chunk& id);
	virtual int initializeTables(fbtBinTables* tables);
	virtual bool skip(const FBTuint32& id);
	virtual int writeData(fbtStream* stream);

	FBTuint32* m_stripList;

	virtual void*   getFBT(void);
	virtual FBTsize getFBTlength(void);
};


#endif//_fbtBlend_h_




#ifdef FBTBLEND_IMPLEMENTATION
#ifndef bftBlend_imp_
#define bftBlend_imp_

//#include "fbtBlend.h"
//#include "fbtTables.h"
//#include "fbtStreams.h"
#ifndef _fbtFile_imp_
#define _fbtFile_imp_

#define FBT_IN_SOURCE

//#include "fbtFile.h"
//#include "fbtStreams.h"
#ifndef _fbtStreams_h_
#define _fbtStreams_h_

//#include "fbtTypes.h"
/** \addtogroup FBT
*  @{
*/

class fbtStream
{
public:
	enum StreamMode
	{
		SM_READ = 1,
		SM_WRITE = 2,
	};

public:
	fbtStream() {}
	virtual ~fbtStream() {}

    virtual void open(const char* /*path*/, fbtStream::StreamMode /*mode*/) {}
	virtual void clear(void) {};


	virtual bool isOpen(void) const = 0;
	virtual bool eof(void) const = 0;

	virtual FBTsize  read(void* dest, FBTsize nr) const = 0;
	virtual FBTsize  write(const void* src, FBTsize nr) = 0;
    virtual FBTsize  writef(const char* /*fmt*/, ...) {return 0;};

	virtual FBTsize  position(void) const = 0;
	virtual FBTsize  size(void) const = 0;

    virtual FBTsize seek(FBTint32 /*off*/, FBTint32 /*way*/) {return 0;}

protected:
    virtual void reserve(FBTsize /*nr*/) {}
};



class           fbtMemoryStream;
typedef void*   fbtFileHandle;


class fbtFileStream : public fbtStream
{
public:
	fbtFileStream();
	~fbtFileStream();

	void open(const char* path, fbtStream::StreamMode mode);
	void close(void);

	bool isOpen(void)   const {return m_handle != 0;}
	bool eof(void)      const;

	FBTsize  read(void* dest, FBTsize nr) const;
	FBTsize  write(const void* src, FBTsize nr);
	FBTsize  writef(const char* buf, ...);


	FBTsize  position(void) const;
	FBTsize  size(void)     const;
	FBTsize seek(FBTint32 off, FBTint32 way);

	void write(fbtMemoryStream &ms) const;

protected:


	fbtFixedString<272> m_file;
	fbtFileHandle       m_handle;
	int                 m_mode;
	int					m_size;
};




#if FBT_USE_GZ_FILE == 1
class fbtGzStream : public fbtStream
{
public:
	fbtGzStream();
	~fbtGzStream();

	void open(const char* path, fbtStream::StreamMode mode);
	void close(void);

	bool isOpen(void)   const {return m_handle != 0;}
	bool eof(void)      const;

	FBTsize  read(void* dest, FBTsize nr) const;
	FBTsize  write(const void* src, FBTsize nr);
	FBTsize  writef(const char* buf, ...);


	FBTsize  position(void) const;
	FBTsize size(void) const;

	// watch it no size / seek

protected:


	fbtFixedString<272> m_file;
	fbtFileHandle       m_handle;
	int                 m_mode;
};
#endif


class fbtMemoryStream : public fbtStream
{
public:
	fbtMemoryStream();
	~fbtMemoryStream();

	// Owns m_buffer: a copy would free it twice.
	fbtMemoryStream(const fbtMemoryStream&) = delete;
	fbtMemoryStream& operator=(const fbtMemoryStream&) = delete;

	void clear(void);

	void open(fbtStream::StreamMode mode);
	void open(const char* path, fbtStream::StreamMode mode);
	void open(const fbtFileStream& fs, fbtStream::StreamMode mode);
	void open(const void* buffer, FBTsize size, fbtStream::StreamMode mode,bool compressed=false);


	bool     isOpen(void)    const   {return m_buffer != 0;}
	bool     eof(void)       const   {return !m_buffer || m_pos >= m_size;}
	FBTsize  position(void)  const   {return m_pos;}
	FBTsize  size(void)      const   {return m_size;}

	FBTsize  read(void* dest, FBTsize nr) const;
	FBTsize  write(const void* src, FBTsize nr);
	FBTsize  writef(const char* buf, ...);
#if FBT_USE_GZ_FILE == 1
	bool gzipInflate( char* inBuf, int inSize);
#endif
#if FBT_USE_ZSTD_FILE == 1
	bool zstdInflate( char* inBuf, int inSize);
#endif
	void*            ptr(void)          {return m_buffer;}
	const void*      ptr(void) const    {return m_buffer;}

	FBTsize seek(FBTint32 off, FBTint32 way);


	void reserve(FBTsize nr);
	void shrinkToFit(void);
protected:
	friend class fbtFileStream;

	// The only place m_buffer is freed outside a reallocation: delete[]s it,
	// nulls it and zeroes size, capacity and position.
	void release(void);

	char*            m_buffer;
	mutable FBTsize  m_pos;
	FBTsize          m_size, m_capacity;
	int              m_mode;
};

/** @}*/
#endif//_fbtStreams_h_

//#include "fbtTables.h"
#ifndef _fbtTables_h_
#define _fbtTables_h_

//#include "fbtTypes.h"

/** \addtogroup FBT
*  @{
*/

#define FBT_MAGIC 4

namespace fbtIdNames
{
    const char FBT_SDNA[FBT_MAGIC] = {'S', 'D', 'N', 'A'};
    const char FBT_NAME[FBT_MAGIC] = {'N', 'A', 'M', 'E'}; // Name array
    const char FBT_TYPE[FBT_MAGIC] = {'T', 'Y', 'P', 'E'}; // Type Array
    const char FBT_TLEN[FBT_MAGIC] = {'T', 'L', 'E', 'N'}; // Type length array
    const char FBT_STRC[FBT_MAGIC] = {'S', 'T', 'R', 'C'}; // Struct/Class Array
    const char FBT_OFFS[FBT_MAGIC] = {'O', 'F', 'F', 'S'}; // Offset map (Optional & TODO)
}

FBT_INLINE fbtFixedString<4> fbtByteToString(FBTuint32 i)
{
	union
	{
		char        ids[4];
		FBTuint32   idi;
	} IDU;
	IDU.idi = i;
	fbtFixedString<4> cp;
	cp.push_back(IDU.ids[0]);
	cp.push_back(IDU.ids[1]);
	cp.push_back(IDU.ids[2]);
	cp.push_back(IDU.ids[3]);
	return cp;
}


typedef struct fbtName
{
	char*           m_name;     // note: memory is in the raw table.
	int             m_loc;
	FBTuint32       m_nameId;
	int             m_ptrCount;
	int             m_numSlots, m_isFptr;
	int             m_arraySize;
	int             m_slots[FBT_ARRAY_SLOTS];
} fbtName;

typedef struct fbtType
{
	char*           m_name;     // note: memory is in the raw table.
	FBTuint32       m_typeId;	// fbtCharHashKey(typeName)
	FBTuint32       m_strcId;	
} fbtType;



typedef union fbtKey32
{
	FBTint16 k16[2];
	FBTint32 k32;
} fbtKey32;

typedef union fbtKey64
{
	FBTuint32 k32[2];
	FBTuint64 k64;

} fbtKey64;


class fbtStruct
{
public:
	typedef fbtArray<fbtStruct> Members;
	typedef fbtArray<fbtKey64>  Keys;
	
	enum Flag
	{
		CAN_LINK    = 0,
		MISSING     = (1 << 0),
		MISALIGNED  = (1 << 1),
		SKIP        = (1 << 2),
		NEED_CAST	= (1 << 3)
	};


	fbtStruct()
		:	m_key(),
			m_val(),
			m_off(0),
			m_len(0),
			m_nr(0),
			m_dp(0),
			m_strcId(0),
			m_flag(0),
			m_members(),
			m_link(0)
	{
	}
	~fbtStruct()    {}


	fbtKey32        m_key;		//k[0]: type, k[1]: name
	fbtKey64        m_val;		//key hash value, k[0]: type hash id, k[1]: member(field) base name hash id or 0(struct)
	FBTint32        m_off;		//offset
	FBTint32        m_len;
	FBTint32        m_nr, m_dp; //nr: array index, dp: embeded depth
	FBTint32        m_strcId;
	FBTint32        m_flag;
	Members         m_members;
	fbtStruct*      m_link;		//file/memory table struct link
	Keys            m_keyChain; //parent key hash chain(0: type hash, 1: name hash), size() == m_dp

	FBTsizeType     getUnlinkedMemberCount();
};


class fbtBinTables
{
public:
	typedef fbtName*                Names;  // < fbtMaxTable
	typedef fbtType*                Types;  // < fbtMaxTable
	typedef FBTtype*                TypeL;  // < fbtMaxTable
	typedef FBTtype**               Strcs;  // < fbtMaxTable * fbtMaxMember;


	// Base name trim (*[0-9]) for partial type, name matching
	// Example: M(char m_var[32]) F(char m_var[24])
	//
	//          result = M(char m_var[24] = F(char m_var[24]) then (M(char m_var[24->32]) = 0)
	//
	// (Note: bParse will skip m_var all together because of 'strcmp(Mtype, Ftype) && strcmp(Mname, Fname)')
	//
	typedef fbtArray<FBTuint32>     NameB;
	typedef fbtArray<fbtStruct*>    OffsM;

	typedef fbtHashTable<fbtCharHashKey, fbtType> TypeFinder;

public:

	fbtBinTables();
	fbtBinTables(void* ptr, const FBTsize& len);
	~fbtBinTables();

	bool read(bool swap);
	bool read(const void* ptr, const FBTsize& len, bool swap);

	FBTtype findTypeId(const fbtCharHashKey &cp);

	const char* getStructType(const fbtStruct* strc);
	const char* getStructName(const fbtStruct* strc);
	const char* getOwnerStructName(const fbtStruct* strc);


	Names   m_name;
	Types   m_type;
	TypeL   m_tlen;
	Strcs   m_strc;
	OffsM   m_offs;
	NameB   m_base;

	FBTuint32 m_nameNr;
	FBTuint32 m_typeNr;
	FBTuint32 m_strcNr;

	// It's safe to assume that memory len is FBT_VOID and file len is FH_CHUNK_64 ? 8 : 4
	// Othewise this library will not even compile (no more need for 'sizeof(ListBase) / 2')
	FBTuint8    m_ptr;
	void*       m_otherBlock;
	FBTsize     m_otherLen;


private:

	TypeFinder m_typeFinder;

	void putMember(FBTtype* cp, fbtStruct* off, FBTtype nr, FBTuint32& cof, FBTuint32 depth, fbtStruct::Keys& keys);
	void compile(FBTtype i, FBTtype nr, fbtStruct* off, FBTuint32& cof, FBTuint32 depth, fbtStruct::Keys& keys);
	void compile(void);
	bool sikp(const FBTuint32& type);

};


/** @}*/
#endif//_fbtTables_h_

//#include "fbtPlatformHeaders.h"
#ifndef _fbtPlatformHeaders_h_
#define _fbtPlatformHeaders_h_

#ifndef FBT_IN_SOURCE
#error source include only!
#endif

#if FBT_PLATFORM == FBT_PLATFORM_WIN32
# if FBT_COMPILER == FBT_COMPILER_MSVC
#   define _WIN32_WINNT 0x403
# endif
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN 1
# endif
# include <windows.h>
# include <io.h>
#else
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <memory.h>

#ifdef _MSC_VER
#pragma warning(disable : 4996)
#endif

#if FBT_COMPILER == FBT_COMPILER_MSVC
#   if _MSC_VER>1310
#       define fbtp_printf(ptr, size, fmt, lst) _vsnprintf_s(ptr, size, fmt, lst)
#   else
#       define fbtp_printf _vsnprintf
#   endif
#else
# define fbtp_printf vsnprintf
#endif

#endif//_fbtPlatformHeaders_h_

// Common Identifiers
const FBTuint32 ENDB = FBT_ID('E', 'N', 'D', 'B');
const FBTuint32 DNA1 = FBT_ID('D', 'N', 'A', '1');
const FBTuint32 DATA = FBT_ID('D', 'A', 'T', 'A');
const FBTuint32 SDNA = FBT_ID('S', 'D', 'N', 'A');


// Compile asserts
FBT_ASSERTCOMP(ChunkLen32, sizeof(fbtFile::Chunk32) == 20);
FBT_ASSERTCOMP(ChunkLen64, sizeof(fbtFile::Chunk64) == 24);

#if FBT_ARCH == FBT_ARCH_32
FBT_ASSERTCOMP(ChunkLenNative, sizeof(fbtFile::Chunk32) == sizeof(fbtFile::Chunk));
#else
FBT_ASSERTCOMP(ChunkLenNative, sizeof(fbtFile::Chunk64) == sizeof(fbtFile::Chunk));
#endif

#define FBT_MALLOC_FAILED   fbtPrintf("Failed to allocate memory!\n");
#define FBT_INVALID_READ    fbtPrintf("Invalid read!\n");
#define FBT_INVALID_LEN     fbtPrintf("Invalid block length!\n");
#define FBT_INVALID_INS     fbtPrintf("Table insertion failed!\n");
#define FBT_LINK_FAILED     fbtPrintf("Linking failed!\n");


struct fbtChunk
{
	enum Size
	{
		BlockSize   = sizeof (fbtFile::Chunk),
		Block32     = sizeof (fbtFile::Chunk32),
		Block64     = sizeof (fbtFile::Chunk64),
	};

	static int read(fbtFile::Chunk* dest, fbtStream* stream, int flags);
	static int write(fbtFile::Chunk* src, fbtStream* stream);
};




fbtFile::fbtFile(const char* uid)
    :   m_uhid(uid), m_aluhid(0), m_version(-1), m_fileVersion(0), m_fileHeader(0),
        m_curFile(0), m_memory(0), m_file(0)
{
}


fbtFile::~fbtFile()
{
	if (m_curFile)
		fbtFree(m_curFile);
	m_curFile = 0;

	MemoryChunk* node = (MemoryChunk*)m_chunks.first, *tnd;
	while (node)
	{
		if (node->m_block)
		{
			//printf("free  m_block: 0x%x\n", node->m_block);fflush(stdout);
			fbtFree(node->m_block);
		}
		if (node->m_newBlock)
		{
			//printf("free m_newBlock: 0x%x\n", node->m_newBlock);fflush(stdout);
			fbtFree(node->m_newBlock);
		}

		tnd  = node;
		node = node->m_next;

		fbtFree(tnd);
	}

	delete m_file;
	delete m_memory;
}

bool fbtFile::FileStartsWith(const char* path,const char* cmp) {
    if (! path || !cmp) return false;
    const size_t numCharsToMatch = strlen(cmp);
    if (numCharsToMatch>255) return false;
    bool match = false;
    FILE* f = UTF8_fopen(path,"rb");
    if (f) {
        if (fseek(f, 0, SEEK_END))  {fclose(f);return match;}
        const long f_size = ftell(f);
        if (f_size<=0 || (size_t)f_size < numCharsToMatch)    {fclose(f);return match;}
        if (fseek(f, 0, SEEK_SET))  {fclose(f);return match;}
        char buff[256]="";
        if (fread(buff,numCharsToMatch,1,f) && strncmp(buff,cmp,numCharsToMatch)==0) match = true;
        fclose(f);
    }
    return match;
}

FILE* fbtFile::UTF8_fopen(const char* filename, const char* mode)   {
    if (!filename || !mode) return NULL;
#	ifdef _WIN32
    const int filenameLen = strlen(filename);
    const int modeLen = strlen(mode);
    wchar_t wfilename[MAX_PATH+1]=L"";
    wchar_t wmode[MAX_PATH+1]=L"";
    FILE* f = NULL;
    int wfilenameLen = 0, wmodeLen = 0;

    if (filenameLen == 0 || modeLen == 0) return NULL;
    wfilenameLen    = MultiByteToWideChar(CP_UTF8, 0, filename, filenameLen,    wfilename,  MAX_PATH);
    wmodeLen        = MultiByteToWideChar(CP_UTF8, 0, mode,     modeLen,        wmode,      MAX_PATH);
    if (wfilenameLen <= MAX_PATH && wmodeLen <= MAX_PATH) {
        wfilename[wfilenameLen] = L'\0';
        wmode[wmodeLen]         = L'\0';
        f = _wfopen(wfilename, wmode);
    }
    else f = fopen(filename, mode); // fallback
    return f;
#	else
    return fopen(filename, mode);
#	endif
}

unsigned char* fbtFile::FBT_GetFileContent(const char *filePath,unsigned long* pSizeOut,const char modes[])   {
    unsigned char* ptr = NULL;
    if (pSizeOut) *pSizeOut=0;
    if (!filePath) return ptr;
    FILE* f;
    if ((f = fbtFile::UTF8_fopen(filePath, modes)) == NULL) return ptr;
    if (fseek(f, 0, SEEK_END))  {fclose(f);return ptr;}
    const long f_size_signed = ftell(f);
    if (f_size_signed == -1)    {fclose(f);return ptr;}
    size_t f_size = (size_t)f_size_signed;
    if (fseek(f, 0, SEEK_SET))  {fclose(f);return ptr;}
    ptr = new unsigned char[f_size];
    if (!ptr) return ptr;
    const size_t f_size_read = f_size>0 ? fread((unsigned char*)ptr, 1, f_size, f) : 0;
    fclose(f);
    if (f_size_read == 0 || f_size_read!=f_size)    {delete[] (ptr);ptr=NULL;}
    else if (pSizeOut) *pSizeOut=f_size;
    return ptr;
}

int fbtFile::parse(const char* path,const char* uncompressedFileDetectorPrefix)    {
    return parse(path,FileStartsWith(path,uncompressedFileDetectorPrefix) ? PM_UNCOMPRESSED : PM_COMPRESSED);
}

int fbtFile::parse(const char* path, int mode)
{
	fbtStream* stream = 0;

#   if ((FBT_USE_ZSTD_FILE==1) && (BLENDER_VERSION>=300))
    if (mode==PM_COMPRESSED)    {
        unsigned long memorySize = 0;
        unsigned char* memory = fbtFile::FBT_GetFileContent(path,&memorySize,"rb");
        int rv = FS_FAILED;
        if (memory) {
            rv = parse((const void*) memory, memorySize, mode, /*bool suppressHeaderWarning*/false);
            delete[] (memory);memory=NULL;memorySize=0;
        }
        if (rv==FS_OK) return rv;
        else {
            fbtPrintf("Warning: fbtFile::parse(\"%s\" has failed with error code %d, when using the zstd decoder.",path,rv);
#           if FBT_USE_GZ_FILE == 1
            fbtPrintf("Trying with the gzip decoder now.");
#           endif
        }
    }
#   endif //((FBT_USE_ZSTD_FILE==1) && (BLENDER_VERSION>=300))

	if (mode == PM_UNCOMPRESSED || mode == PM_COMPRESSED)
	{
#   if FBT_USE_GZ_FILE == 1
		if (mode == PM_COMPRESSED)  {
            stream = new fbtGzStream();
	    }
		else
#   endif  // FBT_USE_GZ_FILE == 1
		{
			stream = new fbtFileStream();
		}
		stream->open(path, fbtStream::SM_READ);
	}
	else
	{
		stream = new fbtMemoryStream();
		stream->open(path, fbtStream::SM_READ);
	}

	if (!stream->isOpen())
	{
		fbtPrintf("File '%s' loading failed\n", path);
		return FS_FAILED;
	}

	if (m_curFile)
		fbtFree(m_curFile);


	FBTsize pl = strlen(path);

	m_curFile = (char*)fbtMalloc(pl + 1);
	if (m_curFile)
	{
		fbtMemcpy(m_curFile, path, pl);
		m_curFile[pl] = 0;
	}

    int result = parseStreamImpl(stream);
	delete stream;
	return result;
}



int fbtFile::parse(const void* memory, FBTsize sizeInBytes, int mode, bool suppressHeaderWarning)
{
	fbtMemoryStream ms;
	ms.open( memory, sizeInBytes, fbtStream::SM_READ, mode==PM_COMPRESSED );

	if (!ms.isOpen())
	{
		fbtPrintf("Memory %p(%i) loading failed\n", memory, sizeInBytes);
		return FS_FAILED;
	}

	return parseStreamImpl(&ms,suppressHeaderWarning);
}



int fbtFile::parseHeader(fbtStream* stream, bool suppressHeaderWarning)
{
	m_header.resize(12);
	stream->read(m_header.ptr(), 12);

	if (!fbtCharNEq(m_header.c_str(), m_uhid, 6) && !fbtCharNEq(m_header.c_str(), m_aluhid, 7))
	{
		if (!suppressHeaderWarning)
			fbtPrintf("Unknown header ID '%s'\n", m_header.c_str());
		return FS_INV_HEADER_STR;
	}

	char* headerMagic = (m_header.ptr() + 7);

	m_fileHeader = 0;
	m_fileVersion = 0;

	if (*(headerMagic++) == FM_64_BIT)
	{
		m_fileHeader |= FH_CHUNK_64;
		if (FBT_VOID4)
			m_fileHeader |= FH_VAR_BITS;
	}
	else if (FBT_VOID8)
		m_fileHeader |= FH_VAR_BITS;


	if (*(headerMagic++) == FM_BIG_ENDIAN)
	{
		if (FBT_ENDIAN_IS_LITTLE)
			m_fileHeader |= FH_ENDIAN_SWAP;
	}
	else if (FBT_ENDIAN_IS_BIG)
		m_fileHeader |= FH_ENDIAN_SWAP;


	m_fileVersion = atoi(headerMagic);

	return FS_OK;
}


int fbtFile::parseStreamImpl(fbtStream* stream, bool suppressHeaderWarning)
{
	int status;

	status = parseHeader(stream,suppressHeaderWarning);
	if (status != FS_OK)
	{
		fbtPrintf("Failed to extract header!\n");
		return status;
	}

	if (!m_memory)
	{
		m_memory = new fbtBinTables();

		status = initializeTables(m_memory);
		if (status != FS_OK)
		{
			fbtPrintf("Failed to initialize builtin tables\n");
			return status;
		}
	}


	if (m_file)
	{
		delete m_file;
		m_file = 0;
	}


	// preallocate table
	m_map.reserve(fbtDefaultAlloc);


	Chunk chunk;


	// Scan chunks

	do
	{
		if ((status = fbtChunk::read(&chunk, stream, m_fileHeader)) <= 0)
		{
			FBT_INVALID_READ;
			return FS_INV_READ;
		}


		if (chunk.m_code == SDNA)
		{
			chunk.m_code = DNA1;
			stream->seek(-status, SEEK_CUR);
			chunk.m_len = stream->size() - stream->position();
		}

		// exit on end byte
		if (chunk.m_code == ENDB)
			break;


		void* curPtr = fbtMalloc(chunk.m_len);
		//printf("alloc curPtr: 0x%x\n", curPtr);fflush(stdout);
		if (!curPtr)
		{
			FBT_MALLOC_FAILED;
			return FS_BAD_ALLOC;
		}

		if (stream->read(curPtr, chunk.m_len) <= 0)
		{
			FBT_INVALID_READ;
			return FS_INV_READ;
		}

		if (chunk.m_code == DNA1)
		{
			m_file = new fbtBinTables(curPtr, chunk.m_len);
			m_file->m_ptr = m_fileHeader & FH_CHUNK_64 ? 8 : 4;


			if (!m_file->read((m_fileHeader & FH_ENDIAN_SWAP) != 0))
			{
				fbtPrintf("Failed to initialize tables\n");
				return FS_INV_READ;
			}

			compileOffsets();

			if ((status = link()) != FS_OK)
			{
				FBT_LINK_FAILED;
				return FS_LINK_FAILED;
			}
			break;
		}
		else
		{
#if FBT_ASSERT_INSERT
			FBTsizeType pos;
			if ((pos = m_map.find(chunk.m_old)) != FBT_NPOS)
			{
				fbtFree(curPtr);
				curPtr = 0;
				int result = fbtMemcmp(&m_map.at(pos)->m_chunk, &chunk, fbtChunk::BlockSize);
				if (result != 0)
				{
					FBT_INVALID_READ;
					return FS_INV_READ;
				}
			}
#else
			if (m_map.find(chunk.m_old) != FBT_NPOS)
			{
				//printf("free  curPtr: 0x%x\n", curPtr);
				fbtFree(curPtr);
				curPtr = 0;
			}
#endif
			else
			{
				MemoryChunk* bin = static_cast<MemoryChunk*>(fbtMalloc(sizeof(MemoryChunk)));
				if (!bin)
				{
					FBT_MALLOC_FAILED;
					return FS_BAD_ALLOC;
				}
				fbtMemset(bin, 0, sizeof(MemoryChunk));
				bin->m_block = curPtr;

				Chunk* cp    = &bin->m_chunk;
				cp->m_code   = chunk.m_code;
				cp->m_len    = chunk.m_len;
				cp->m_nr     = chunk.m_nr;
				cp->m_typeid = chunk.m_typeid;
				cp->m_old    = chunk.m_old;
				m_chunks.push_back(bin);

				if (m_map.insert(bin->m_chunk.m_old, bin) == false)
				{
					FBT_INVALID_INS;
					return FS_INV_INSERT;
				}
			}
		}
	}
	while (!stream->eof());
	return status;
}


class fbtPrimType
{
public:

};

class fbtLinkCompiler
{
public:
	fbtBinTables* m_mp;
	fbtBinTables* m_fp;

	fbtStruct* find(const fbtCharHashKey& kvp);
	fbtStruct* find(fbtStruct* strc, fbtStruct* member, bool isPointer, bool& needCast);
	int        link(void);
};



fbtStruct* fbtLinkCompiler::find(const fbtCharHashKey& kvp)
{
	FBTtype i;
	if ( (i = m_fp->findTypeId(kvp)) != ((FBTtype)-1))
		return m_fp->m_offs.at(i);
	return 0;
}


fbtStruct* fbtLinkCompiler::find(fbtStruct* strc, fbtStruct* member, bool isPointer, bool& needCast)
{
	fbtStruct::Members::Pointer md = strc->m_members.ptr();
	FBTsizeType i, s = strc->m_members.size();

	FBTuint32 k1 = member->m_val.k32[0];

	for (i = 0; i < s; i++)
	{
		fbtStruct* strc2 = &md[i];

		if (strc2->m_nr == member->m_nr && strc2->m_dp ==  member->m_dp)
		{
			if (strc2->m_val.k32[1] == member->m_val.k32[1]) //base name
			{				
				if (!strc2->m_keyChain.equal(member->m_keyChain))
					continue;

				FBTuint32 k2 = strc2->m_val.k32[0];
				if (k1 == k2)
					return strc2;

				if (isPointer)
					continue;

				if (fbtIsIntType(k1) && fbtIsIntType(k2))
					return strc2;

				if (fbtIsNumberType(k1) && fbtIsNumberType(k2))
				{
					needCast = true;
					return strc2;
				}
			}
		}
	}


	return 0;
}

int fbtLinkCompiler::link(void)
{
	fbtBinTables::OffsM::Pointer md = m_mp->m_offs.ptr();
	fbtBinTables::OffsM::Pointer fd = m_fp->m_offs.ptr();

	FBTsizeType i, i2; 
	fbtStruct::Members::Pointer p2;


	for (i = 0; i < m_mp->m_offs.size(); ++i)
	{
		fbtStruct* strc = md[i];
		strc->m_link = find(m_mp->m_type[strc->m_key.k16[0]].m_name);

		if (strc->m_link)
			strc->m_link->m_link = strc;

		p2 = strc->m_members.ptr();

		//fbtPrintf("+%-3d %s\n", i, m_mp->getStructType(strc));
		for (i2 = 0; i2 < strc->m_members.size(); ++i2)
		{
			fbtStruct* member = &strc->m_members[i2];
			//fbtPrintf("  %3d %s %s\n", i2, m_mp->getStructType(strc2), m_mp->getStructName(strc2));

			FBT_ASSERT(!member->m_link);
			member->m_flag |= strc->m_link ? 0 : fbtStruct::MISSING;

			if (!(member->m_flag & fbtStruct::MISSING))
			{
				FBT_ASSERT(member->m_key.k16[1] < m_mp->m_nameNr);
				bool isPointer = m_mp->m_name[member->m_key.k16[1]].m_ptrCount > 0;
				bool needCast = false;
				member->m_link = find(strc->m_link, member, isPointer, needCast);
				if (member->m_link)
				{
					member->m_link->m_link = member;
					if (needCast)
					{
						member->m_flag |= fbtStruct::NEED_CAST;
						member->m_link->m_flag |= fbtStruct::NEED_CAST;
					}
				}
			}
		}

	}

	return fbtFile::FS_OK;
}

void copyValues(FBTbyte* srcPtr, FBTbyte* dstPtr, FBTsize srcElmSize, FBTsize dstElmSize, FBTsize len)
{
	FBTsize i;
	for (i = 0; i < len; i++)
	{
		fbtMemcpy(srcPtr, dstPtr, srcElmSize);
		srcPtr += srcElmSize;
		dstElmSize += dstElmSize;
	}
}

void castValue(FBTsize* srcPtr, FBTsize* dstPtr, FBT_PRIM_TYPE srctp, FBT_PRIM_TYPE dsttp, FBTsize len)
{
#define GET_V(value, current, type, cast, ptr, match) \
	if (current == type) \
	{ \
		value = (*(cast*)ptr); \
		ptr += sizeof(cast); \
		if (++match >= 2) continue; \
	}

#define SET_V(value, current, type, cast, ptr, match) \
	if (current == type) \
	{ \
		(*(cast*)ptr) = (cast)value; \
		ptr += sizeof(cast); \
		if (++match >= 2) continue; \
	}

	double value = 0.0;

	FBTsizeType i;
	for (i = 0; i < len; i++)
	{
		int match = 0;
		GET_V(value, srctp, FBT_PRIM_CHAR,    char,            srcPtr, match);
		SET_V(value, dsttp, FBT_PRIM_CHAR,    char,            dstPtr, match);
		GET_V(value, srctp, FBT_PRIM_SHORT,   short,           srcPtr, match);
		SET_V(value, dsttp, FBT_PRIM_SHORT,   short,           dstPtr, match);
		GET_V(value, srctp, FBT_PRIM_USHORT,  unsigned short,  srcPtr, match);
		SET_V(value, dsttp, FBT_PRIM_USHORT,  unsigned short,  dstPtr, match);
		GET_V(value, srctp, FBT_PRIM_INT,     int,             srcPtr, match);
		SET_V(value, dsttp, FBT_PRIM_INT,     int,             dstPtr, match);
		GET_V(value, srctp, FBT_PRIM_LONG,    int,             srcPtr, match);
		SET_V(value, dsttp, FBT_PRIM_LONG,    int,             dstPtr, match);
		GET_V(value, srctp, FBT_PRIM_FLOAT,   float,           srcPtr, match);
		SET_V(value, dsttp, FBT_PRIM_FLOAT,   float,           dstPtr, match);
		GET_V(value, srctp, FBT_PRIM_DOUBLE,  double,          srcPtr, match);
		SET_V(value, dsttp, FBT_PRIM_DOUBLE,  double,          dstPtr, match);
	}
#undef GET_V
#undef SET_V
}


int fbtFile::link(void)
{
	fbtBinTables::OffsM::Pointer md = m_memory->m_offs.ptr();
	fbtBinTables::OffsM::Pointer fd = m_file->m_offs.ptr();
	FBTsizeType s2, i2, a2, n;
	fbtStruct::Members::Pointer p2;
	FBTsize mlen, malen, total, pi;

	char* dst, *src;
	FBTsize* dstPtr, *srcPtr;

	bool endianSwap = (m_fileHeader & FH_ENDIAN_SWAP) != 0;

	static const FBThash hk = fbtCharHashKey("Link").hash();


	MemoryChunk* node;
	for (node = (MemoryChunk*)m_chunks.first; node; node = node->m_next)
	{
		if (node->m_chunk.m_typeid > m_file->m_strcNr || !( fd[node->m_chunk.m_typeid]->m_link))
			continue;

		fbtStruct* fs, *ms;
		fs = fd[node->m_chunk.m_typeid];
		ms = fs->m_link;

		node->m_newTypeId = ms->m_strcId;

		if (m_memory->m_type[ms->m_key.k16[0]].m_typeId == hk)
		{
			FBTsize totSize = node->m_chunk.m_len;
			node->m_newBlock = fbtMalloc(totSize);
			//printf("alloc1 m_newBlock: 0x%x %d\n", node->m_newBlock, totSize);fflush(stdout);

			if (!node->m_newBlock)
			{
				FBT_MALLOC_FAILED;
				return FS_BAD_ALLOC;
			}

			fbtMemcpy(node->m_newBlock, node->m_block, totSize);
			continue;
		}


		if (skip(m_memory->m_type[ms->m_key.k16[0]].m_typeId))
			continue;


		FBTsize totSize = (node->m_chunk.m_nr * ms->m_len);

		node->m_chunk.m_len = totSize;


		node->m_newBlock = fbtMalloc(totSize);
		//printf("alloc2 m_newBlock: 0x%x %d\n", node->m_newBlock, totSize);fflush(stdout);

		if (!node->m_newBlock)
		{
			FBT_MALLOC_FAILED;
			return FS_BAD_ALLOC;
		}



		// always zero this
		fbtMemset(node->m_newBlock, 0, totSize);
	}



	FBTuint8 mps = m_memory->m_ptr, fps = m_file->m_ptr;

	for (node = (MemoryChunk*)m_chunks.first; node; node = node->m_next)
	{
		if (node->m_newTypeId > m_memory->m_strcNr)
			continue;



		fbtStruct* cs = md[node->m_newTypeId];
		if (m_memory->m_type[cs->m_key.k16[0]].m_typeId == hk)
			continue;

		if (!cs->m_link || skip(m_memory->m_type[cs->m_key.k16[0]].m_typeId) || !node->m_newBlock)
		{
			//printf("free  m_newBlock: 0x%x \n", node->m_newBlock);fflush(stdout);

			fbtFree(node->m_newBlock);
			node->m_newBlock = 0;

			continue;
		}

		s2 = cs->m_members.size();
		p2 = cs->m_members.ptr();

		for (n = 0; n < node->m_chunk.m_nr; ++n)
		{
			dst = static_cast<char*>(node->m_newBlock) + (cs->m_len * n);
			src = static_cast<char*>(node->m_block) + (cs->m_link->m_len * n);


			for (i2 = 0; i2 < s2; ++i2)
			{
				fbtStruct* dstStrc = &p2[i2];
				fbtStruct* srcStrc = dstStrc->m_link;

				// If it's missing we can safely skip this block
				if (!srcStrc)
					continue;


				dstPtr = reinterpret_cast<FBTsize*>(dst + dstStrc->m_off);
				srcPtr = reinterpret_cast<FBTsize*>(src + srcStrc->m_off);



				const fbtName& nameD = m_memory->m_name[dstStrc->m_key.k16[1]];
				const fbtName& nameS = m_file->m_name[srcStrc->m_key.k16[1]];


				if (nameD.m_ptrCount > 0)
				{
					if ((*srcPtr))
					{
						if (nameD.m_ptrCount  > 1)
						{
							MemoryChunk* bin = findBlock((FBTsize)(*srcPtr));
							if (bin)
							{
								if (bin->m_flag & MemoryChunk::BLK_MODIFIED)
									(*dstPtr) = (FBTsize)bin->m_newBlock;
								else
								{
									// take pointer size out of the equation
									total = bin->m_chunk.m_len / fps;


									FBTsize* nptr = (FBTsize*)fbtMalloc(total * mps);
									fbtMemset(nptr, 0, total * mps);

									// always use 32 bit, then offset + 2 for 64 bit (Old pointers are sorted in this mannor)
									FBTuint32* optr = (FBTuint32*)bin->m_block;


									for (pi = 0; pi < total; pi++, optr += (fps == 4 ? 1 : 2))
										nptr[pi] = (FBTsize)findPtr((FBTsize) * optr);

									(*dstPtr) = (FBTsize)(nptr);

									bin->m_chunk.m_len = total * mps;
									bin->m_flag |= MemoryChunk::BLK_MODIFIED;

									fbtFree(bin->m_newBlock);
									bin->m_newBlock = nptr;
								}
							}
							else
							{
								//fbtPrintf("**block not found @ 0x%p)\n", src);
							}
						}
						else
						{
							malen = nameD.m_arraySize > nameS.m_arraySize ? nameS.m_arraySize : nameD.m_arraySize;

							FBTsize* dptr = (FBTsize*)dstPtr;

							// always use 32 bit, then offset + 2 for 64 bit (Old pointers are sorted in this mannor)
							FBTuint32* sptr = (FBTuint32*)srcPtr;


							for (a2 = 0; a2 < malen; ++a2, sptr += (fps == 4 ? 1 : 2))
								dptr[a2] = (FBTsize)findPtr((FBTsize) * sptr);
						}
					}
				}
				else
				{
					FBTsize dstElmSize = dstStrc->m_len / nameD.m_arraySize;
					FBTsize srcElmSize = srcStrc->m_len / nameS.m_arraySize;

					bool needCast = (dstStrc->m_flag & fbtStruct::NEED_CAST) != 0;
					bool needSwap = endianSwap && srcElmSize > 1;

					if (!needCast && !needSwap && srcStrc->m_val.k32[0] == dstStrc->m_val.k32[0]) //same type
					{						
						// Take the minimum length of any array.
						mlen = fbtMin(srcStrc->m_len, dstStrc->m_len);

						fbtMemcpy(dstPtr, srcPtr, mlen);
						continue;
					}

					FBTbyte* dstBPtr = reinterpret_cast<FBTbyte*>(dstPtr);
					FBTbyte* srcBPtr = reinterpret_cast<FBTbyte*>(srcPtr);

					FBT_PRIM_TYPE stp = FBT_PRIM_UNKNOWN, dtp  = FBT_PRIM_UNKNOWN;

					if (needCast || needSwap)
					{
						stp = fbtGetPrimType(srcStrc->m_val.k32[0]);
						dtp = fbtGetPrimType(dstStrc->m_val.k32[0]);

						FBT_ASSERT(fbtIsNumberType(stp) && fbtIsNumberType(dtp) && stp != dtp);
					}

					FBTsize alen = fbtMin(nameS.m_arraySize, nameD.m_arraySize);
					FBTsize elen = fbtMin(srcElmSize, dstElmSize);

					FBTbyte tmpBuf[8] = {0, };
					FBTsize i;
					for (i = 0; i < alen; i++)
					{
						FBTbyte* tmp = srcBPtr;
						if (needSwap)
						{
							tmp = tmpBuf;
							fbtMemcpy(tmpBuf, srcBPtr, srcElmSize);

							if (stp == FBT_PRIM_SHORT || stp == FBT_PRIM_USHORT) 
								fbtSwap16((FBTuint16*)tmpBuf, 1);
							else if (stp >= FBT_PRIM_INT && stp <= FBT_PRIM_FLOAT) 
								fbtSwap32((FBTuint32*)tmpBuf, 1);
							else if (stp == FBT_PRIM_DOUBLE)
								fbtSwap64((FBTuint64*)tmpBuf, 1);
							else
								fbtMemset(tmpBuf, 0, sizeof(tmpBuf)); //unknown type
						}
						
						if (needCast)
							castValue((FBTsize*)tmp, (FBTsize*)dstBPtr, stp, dtp, 1);
						else
							fbtMemcpy(dstBPtr, tmp, elen);

						dstBPtr += dstElmSize;
						srcBPtr += srcElmSize;
					}
				}
			}
		}

		notifyData(node->m_newBlock, node->m_chunk);
	}



	for (node = (MemoryChunk*)m_chunks.first; node; node = node->m_next)
	{
		if (node->m_block)
		{
			fbtFree(node->m_block);
			node->m_block = 0;
		}
	}

	return fbtFile::FS_OK;
}




void* fbtFile::findPtr(const FBTsize& iptr)
{
	FBTsizeType i;
	if ((i = m_map.find(iptr)) != FBT_NPOS)
		return m_map.at(i)->m_newBlock;
	return 0;
}


fbtFile::MemoryChunk* fbtFile::findBlock(const FBTsize& iptr)
{
	FBTsizeType i;
	if ((i = m_map.find(iptr)) != FBT_NPOS)
		return m_map.at(i);
	return 0;
}



int fbtFile::compileOffsets(void)
{
	fbtLinkCompiler lnk;
	lnk.m_mp = m_memory;
	lnk.m_fp = m_file;
	return lnk.link();
}

bool fbtFile::_setuid(const char* uid)
{
	if (!uid || strlen(uid) != 7) return false;

	m_uhid = uid;
	return true;
}

int fbtFile::reflect(const char* path, const int mode, const fbtEndian& /*endian*/)
{
	fbtStream* fs;
	
#if FBT_USE_GZ_FILE == 1
	if (mode == PM_COMPRESSED)
		fs = new fbtGzStream();
	else
#endif
	{
		fs = new fbtFileStream();
	}
	
	fs->open(path, fbtStream::SM_WRITE);


	FBTuint8 cp = FBT_VOID8 ? FM_64_BIT : FM_32_BIT;
	FBTuint8 ce = ((FBTuint8)fbtGetEndian()) == FBT_ENDIAN_IS_BIG ? FM_BIG_ENDIAN : FM_LITTLE_ENDIAN;

//	Commented for now since the rest of the code does not care
//	if (endian != FBT_ENDIAN_NATIVE)
//	{
//		if (endian == FBT_ENDIAN_IS_BIG)
//			ce = FM_BIG_ENDIAN;
//		else
//			ce = FM_LITTLE_ENDIAN;
//	}

	// put magic
	//fs->writef("%s%c%c%i", m_uhid, cp, ce, m_version);
	char header[12] = {0,0,0,0,0,0,0,0,0,0,0,0};
	char version[33];
	sprintf(version, "%i",m_version);
	
	strncpy(&header[0], m_uhid, 7); // 7 first bytes of header
	header[7] = cp;					// 8th byte = pointer size
	header[8] = ce;					// 9th byte = endianness
	strncpy(&header[9], version, 3);// last 3 bytes vor 3 version char
	
	fs->write(header, 12);
	
	writeData(fs);

	// write DNA1
	Chunk ch;
	ch.m_code   = DNA1;
	ch.m_len    = getFBTlength();
	ch.m_nr     = 1;
	ch.m_old    = 0;
	ch.m_typeid = 0;
	fs->write(&ch, fbtChunk::BlockSize);
	fs->write(getFBT(), ch.m_len);


	// write ENDB (End Byte | EOF )
	ch.m_code   = ENDB;
	ch.m_len    = 0;
	ch.m_nr     = 0;
	ch.m_old    = 0;
	ch.m_typeid = 0;
	fs->write(&ch, fbtChunk::BlockSize);
	
	delete fs;
	return FS_OK;

}

void fbtFile::writeStruct(fbtStream* stream, FBTtype index, FBTuint32 code, FBTsize len, void* writeData)
{
	Chunk ch;
	ch.m_code   = code;
	ch.m_len    = len;
	ch.m_nr     = 1;
	ch.m_old    = (FBTsize)writeData;
	ch.m_typeid = index;

	fbtChunk::write(&ch, stream);
}

void fbtFile::writeBuffer(fbtStream* stream, FBTsize len, void* writeData)
{
	Chunk ch;
	ch.m_code   = DATA;
	ch.m_len    = len;
	ch.m_nr     = 1;
	ch.m_old    = (FBTsize)writeData;
	ch.m_typeid = m_memory->findTypeId("Link");

	fbtChunk::write(&ch, stream);
}

int fbtChunk::write(fbtFile::Chunk* src, fbtStream* stream)
{	
	int size = 0;
	size += stream->write(src, BlockSize);
	size += stream->write((void*)src->m_old, src->m_len);
	return size;
}

int fbtChunk::read(fbtFile::Chunk* dest, fbtStream* stream, int flags)
{
	int bytesRead = 0;
	bool bitsVary   = (flags & fbtFile::FH_VAR_BITS) != 0;
	bool swapEndian = (flags & fbtFile::FH_ENDIAN_SWAP) != 0;

	fbtFile::Chunk64 c64;
	fbtFile::Chunk32 c32;
	fbtFile::Chunk*  cpy;


	if (FBT_VOID8)
	{

		if (bitsVary)
		{
			fbtFile::Chunk32 src;
			if ((bytesRead = stream->read(&src, Block32)) <= 0)
			{
				FBT_INVALID_READ;
				return fbtFile::FS_INV_READ;
			}

			c64.m_code    = src.m_code;
			c64.m_len     = src.m_len;

			union
			{
				FBTuint64   m_ptr;
				FBTuint32   m_doublePtr[2];
			} ptr;
			ptr.m_doublePtr[0] = src.m_old;
			ptr.m_doublePtr[1] = 0;

			c64.m_old     = ptr.m_ptr;
			c64.m_typeid  = src.m_typeid;
			c64.m_nr      = src.m_nr;
		}
		else
		{
			if ((bytesRead = stream->read(&c64, BlockSize)) <= 0)
			{
				FBT_INVALID_READ;
				return fbtFile::FS_INV_READ;
			}
		}


		if (swapEndian)
		{
			if ((c64.m_code & 0xFFFF) == 0)
				c64.m_code >>= 16;

			c64.m_len    = fbtSwap32(c64.m_len);
			c64.m_nr     = fbtSwap32(c64.m_nr);
			c64.m_typeid = fbtSwap32(c64.m_typeid);
		}



		cpy = (fbtFile::Chunk*)(&c64);
	}
	else
	{

		if (bitsVary)
		{
			fbtFile::Chunk64 src;
			if ((bytesRead = stream->read(&src, Block64)) <= 0)
			{
				FBT_INVALID_READ;
				return fbtFile::FS_INV_READ;
			}

			// copy down

			c32.m_code    = src.m_code;
			c32.m_len     = src.m_len;

			union
			{
				FBTuint64   m_ptr;
				FBTuint32   m_doublePtr[2];
			} ptr;
			ptr.m_doublePtr[0] = 0;
			ptr.m_doublePtr[1] = 0;
			ptr.m_ptr = src.m_old;

			c32.m_old       = ptr.m_doublePtr[0] != 0 ? ptr.m_doublePtr[0] : ptr.m_doublePtr[1];
			c32.m_typeid    = src.m_typeid;
			c32.m_nr        = src.m_nr;
		}
		else
		{
			if ((bytesRead = stream->read(&c32, BlockSize)) <= 0)
			{
				FBT_INVALID_READ;
				return fbtFile::FS_INV_READ;
			}
		}


		if (swapEndian)
		{
			if ((c32.m_code & 0xFFFF) == 0)
				c32.m_code >>= 16;

			c32.m_len    = fbtSwap32(c32.m_len);
			c32.m_nr     = fbtSwap32(c32.m_nr);
			c32.m_typeid = fbtSwap32(c32.m_typeid);
		}



		cpy = (fbtFile::Chunk*)(&c32);
	}

	if (cpy->m_len == FBT_NPOS)
	{
		FBT_INVALID_LEN;
		return fbtFile::FS_INV_LENGTH;
	}

	fbtMemcpy(dest, cpy, BlockSize);
	return bytesRead;
}


// Source File "fbtStreams.cpp"
//#define FBT_IN_SOURCE
//#include "fbtStreams.h"
//#include "fbtPlatformHeaders.h"


#if FBT_USE_GZ_FILE == 1
#include "zlib.h"
#include "zconf.h"
#endif

#if FBT_USE_ZSTD_FILE == 1
#include "zstd.h"
#endif

fbtFileStream::fbtFileStream() 
	:    m_file(), m_handle(0), m_mode(0), m_size(0)
{
}


fbtFileStream::~fbtFileStream()
{
	close();
}



void fbtFileStream::open(const char* p, fbtStream::StreamMode mode)
{
	if (m_handle != 0 && m_file != p)
		fclose((FILE *)m_handle);

	char fm[3] = {0, 0, 0};
	char* mp = &fm[0];
	if (mode & fbtStream::SM_READ)
		*mp++ = 'r';
	else if (mode & fbtStream::SM_WRITE)
		*mp++ = 'w';
	*mp++ = 'b';
	fm[2] = 0;

	m_file = p;
    m_handle = fbtFile::UTF8_fopen(m_file.c_str(), fm);

	if (m_handle && (mode & fbtStream::SM_READ))
	{
		FILE *fp = (FILE*)m_handle;

		position();
		fseek(fp, 0, SEEK_END);
		m_size = ftell(fp);
		fseek(fp, 0, SEEK_SET);
	}


}


void fbtFileStream::close(void)
{
	if (m_handle != 0)
	{
		fclose((FILE*)m_handle);
		m_handle = 0;
	}

	m_file.clear();
}


bool fbtFileStream::eof(void) const
{
	if (!m_handle)
		return true;
	return feof((FILE*)m_handle) != 0;
}

FBTsize fbtFileStream::position(void) const
{
	return ftell((FILE*)m_handle);
}


FBTsize fbtFileStream::size(void) const
{
	return m_size;
}

FBTsize fbtFileStream::seek(FBTint32 off, FBTint32 way)
{
	if (!m_handle)
		return 0;

	return fseek((FILE*)m_handle, off, way);
}


FBTsize fbtFileStream::read(void* dest, FBTsize nr) const
{
	if (m_mode == fbtStream::SM_WRITE) 
		return -1;

	if (!dest || !m_handle) 
		return -1;

	return fread(dest, 1, nr, (FILE *)m_handle);
}



FBTsize fbtFileStream::write(const void* src, FBTsize nr)
{
	if (m_mode == fbtStream::SM_READ) return -1;
	if (!src || !m_handle) return -1;

	return fwrite(src, 1, nr, (FILE*)m_handle);
}

void fbtFileStream::write(fbtMemoryStream &ms) const
{
	FILE *fp = (FILE*)m_handle;

	int oldPos = ftell(fp);

	fseek(fp, 0, SEEK_END);
	int len = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	ms.reserve(len + 1);
	ms.m_size = read(ms.m_buffer, len);

	fseek(fp, oldPos, SEEK_SET);
}


FBTsize fbtFileStream::writef(const char* fmt, ...)
{
	static char tmp[1024];

	va_list lst;
	va_start(lst, fmt);
	int size = fbtp_printf(tmp, 1024, fmt, lst);
	va_end(lst);

	if (size > 0)
	{
		tmp[size] = 0;
		return write(tmp, size);
	}
	return -1;

}


#if FBT_USE_GZ_FILE == 1

fbtGzStream::fbtGzStream() 
	:    m_file(), m_handle(0), m_mode(0)
{
}


fbtGzStream::~fbtGzStream()
{
	close();
}


void fbtGzStream::open(const char* p, fbtStream::StreamMode mode)
{
	if (m_handle != 0 && m_file != p)
        gzclose((gzFile) m_handle);

	char fm[3] = {0, 0, 0};
	char* mp = &fm[0];
	if (mode & fbtStream::SM_READ)
		*mp++ = 'r';
	else if (mode & fbtStream::SM_WRITE)
		*mp++ = 'w';
	*mp++ = 'b';
	fm[2] = 0;

	m_file = p;
	m_handle = gzopen(m_file.c_str(), fm);
}


void fbtGzStream::close(void)
{
	if (m_handle != 0)
	{
        gzclose((gzFile)m_handle);
		m_handle = 0;
	}

	m_file.clear();
}



FBTsize fbtGzStream::read(void* dest, FBTsize nr) const
{
	if (m_mode == fbtStream::SM_WRITE) return -1;
	if (!dest || !m_handle) 
		return -1;

    return gzread((gzFile)m_handle, dest, nr);
}


FBTsize fbtGzStream::write(const void* src, FBTsize nr)
{
	if (m_mode == fbtStream::SM_READ) return -1;
	if (!src || !m_handle) return -1;
    return gzwrite((gzFile)m_handle,(const voidp) src, nr);
}


bool fbtGzStream::eof(void) const
{
	if (!m_handle)
		return true;
    return gzeof((gzFile)m_handle) != 0;
}

FBTsize fbtGzStream::position(void) const
{
    return gztell((gzFile)m_handle);
}

FBTsize fbtGzStream::size(void) const
{
	return 0;
}


FBTsize fbtGzStream::writef(const char* fmt, ...)
{
	static char tmp[1024];

	va_list lst;
	va_start(lst, fmt);
	int size = fbtp_printf(tmp, 1024, fmt, lst);
	va_end(lst);

	if (size > 0)
	{
		tmp[size] = 0;
		return write(tmp, size);
	}
	return -1;

}

#endif // FBT_USE_GZ_FILE


fbtMemoryStream::fbtMemoryStream()
	:   m_buffer(0), m_pos(0), m_size(0), m_capacity(0), m_mode(0)
{
}


void fbtMemoryStream::open(fbtStream::StreamMode mode)
{
	m_mode = mode;
}

void fbtMemoryStream::open(const char* path, fbtStream::StreamMode mode)
{
	fbtFileStream fs;
	fs.open(path, fbtStream::SM_READ);

	if (fs.isOpen()) 
		open(fs, mode);
}


void fbtMemoryStream::open(const fbtFileStream& fs, fbtStream::StreamMode mode)
{
	if (fs.isOpen())
	{
		fs.write(*this);
		m_mode = mode;
	}
}


void fbtMemoryStream::open(const void* buffer, FBTsize size, fbtStream::StreamMode mode, bool compressed)
{
	if (buffer && size > 0 && size != FBT_NPOS)
	{
		release();
		m_mode = mode;
		m_pos  = 0;


		if (!compressed)
		{
			m_size = size;
			reserve(m_size);
			fbtMemcpy(m_buffer, buffer, m_size);

		} else
		{
		    bool result = false;
#           if BLENDER_VERSION >= 300
#               if FBT_USE_ZSTD_FILE == 1
                if (!result) result = zstdInflate((char*)buffer,size);
#               endif
#               if FBT_USE_GZ_FILE == 1
                if (!result) result = gzipInflate((char*)buffer,size);
#               endif
#           else //BLENDER_VERSION    		    
#               if FBT_USE_GZ_FILE == 1
                if (!result) result = gzipInflate((char*)buffer,size);
#               endif
#               if FBT_USE_ZSTD_FILE == 1
                if (!result) result = zstdInflate((char*)buffer,size);
#               endif
#           endif //BLENDER_VERSION
		}

	}
}

#if FBT_USE_GZ_FILE == 1
// this method was adapted from this snippet:
// http://windrealm.org/tutorials/decompress-gzip-stream.php
bool fbtMemoryStream::gzipInflate( char* inBuf, int inSize) {
  // 'inBuf' is the whole compressed stream, of compressed size 'inSize'
  release();

  reserve(m_capacity+inSize);

  z_stream strm;
  strm.next_in = (Bytef *) inBuf;
  strm.avail_in = inSize ;
  strm.total_out = 0;
  strm.zalloc = Z_NULL;
  strm.zfree = Z_NULL;

  bool done = false;

  if (inflateInit2(&strm, (16+MAX_WBITS)) != Z_OK) {
    release();
    return false;
  }

  while (!done) {
    // If our output buffer is too small
    if (strm.total_out >= m_capacity ) {
      // Increase size of output buffer; reserve() keeps m_size bytes.
      // Geometric, so the bytes copied stay linear in the output size.
      m_size = strm.total_out;
      reserve(m_capacity+m_capacity/2+(inSize/2));
    }

    strm.next_out = (Bytef *) (m_buffer + strm.total_out);
    strm.avail_out = m_capacity - strm.total_out;

    // Inflate another chunk.
    int err = inflate (&strm, Z_SYNC_FLUSH);
    if (err == Z_STREAM_END) done = true;
    else if (err != Z_OK)  {
      break;
    }
  }

  // !done: inflate stopped on an error before the end of the stream
  if (inflateEnd (&strm) != Z_OK || !done) {
    release();
    return false;
  }
  else {
    m_size = strm.total_out;
    shrinkToFit();
  }

  return done;
}
#endif

#if FBT_USE_ZSTD_FILE == 1
bool fbtMemoryStream::zstdInflate(char* inBuf, int inSize) {
    // 'inBuf' is the whole compressed stream, of compressed size 'inSize'  
    release();
    if ( inSize == 0 ) {
          return false;
    }
    
    const unsigned char* memoryBuffer = (const unsigned char*) inBuf;
    const size_t memoryBufferSize = (size_t) inSize;
  
    // FIll m_size, m_capacity and m_buffer from memoryBuffer and memoryBufferSize.
  
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    const size_t capacityStepMultiplier = 4;   
    
    // input buffer
    const size_t minInputSizeStep = ZSTD_DStreamInSize();
    const size_t inputSizeStep = minInputSizeStep*capacityStepMultiplier;
    ZSTD_inBuffer input = { memoryBuffer, 0, 0 };
    input.size = inputSizeStep<=memoryBufferSize?inputSizeStep:memoryBufferSize;
    
    // output buffer
    const size_t minOutputCapacityStep = ZSTD_DStreamOutSize();
    const size_t outputCapacityStep = minOutputCapacityStep*capacityStepMultiplier;
    reserve(memoryBufferSize*3);   // initial capacity
    ZSTD_outBuffer output = {m_buffer, m_capacity, 0 };
    
    size_t lastRet = 0;
    int failure = 0;
    
    //size_t last_input_pos = 1;    // optional loop lock prevention [line 1/3]
    //size_t num_reallocs = 0, num_decompression_calls = 0;   // optional debug [line 1/4]
    while (input.pos < memoryBufferSize) {
        //if (lastRet==0 && last_input_pos==input.pos) break;    // optional loop lock prevention [line 2/3]
        if (input.size<memoryBufferSize && input.size-input.pos<minInputSizeStep)    {
            input.size+=inputSizeStep;
            if (input.size>memoryBufferSize) input.size=memoryBufferSize;
        }
        size_t const ret = ZSTD_decompressStream(dctx, &output , &input);
        //++num_decompression_calls;   // optional debug [line 2/4]
        if (ZSTD_isError(ret))    {
            fbtPrintf("ZSTD_decompressStream(...) Error: %s\n",ZSTD_getErrorName(ret));
            failure = 1;break;
        }
        else {
            m_size=output.pos;
            if (input.pos < input.size) {
                if (output.size-output.pos<=outputCapacityStep)    {
                    reserve(m_capacity+minOutputCapacityStep+m_capacity/4);     // grow strategy               
                    //++num_reallocs;   // optional debug [line 3/4]
                    output.dst=m_buffer;output.size=m_capacity; // output.pos unchanged           
                }
            }
                
            //fbtPrintf("output: %zu/%zu. input: %zu/%zu.\n",output.pos,output.size,input.pos,input.size);
        }    
        lastRet = ret;
    }
  
    //fbtPrintf("num_reallocs: %zu. num_decompression_calls: %zu.\n",num_reallocs,num_decompression_calls);   // optional debug [line 4/4]
        

    if (lastRet != 0) {
        /* The last return value from ZSTD_decompressStream did not end on a
         * frame, but we reached the end of the file! We assume this is an
         * error, and the input was truncated.
         */
        fbtPrintf("ZSTD_decompressStream(...) Error: EOF before end of stream: %zu\n", lastRet);
        failure = 1;
    }  
    if (failure) {
      release();
    }          
    else shrinkToFit();
    
    ZSTD_freeDCtx(dctx);dctx=NULL;  
    return !failure;
}
#endif


fbtMemoryStream::~fbtMemoryStream()
{
	release();
}


void fbtMemoryStream::release(void)
{
	delete [] m_buffer;
	m_buffer = 0;
	m_size = m_pos = 0;
	m_capacity = 0;
}



void fbtMemoryStream::clear(void)
{
	m_size = m_pos = 0;
	if (m_buffer)
		m_buffer[0] = 0;
}


FBTsize fbtMemoryStream::seek(FBTint32 off, FBTint32 way)
{
	if (way == SEEK_SET)
		m_pos = fbtClamp<FBTsize>(off, 0, m_size);
	else if (way == SEEK_CUR)
		m_pos = fbtClamp<FBTsize>(m_pos + off, 0, m_size);
	else if (way == SEEK_END)
		m_pos = m_size;
	return m_pos;
}



FBTsize fbtMemoryStream::read(void* dest, FBTsize nr) const
{
	if (m_mode == fbtStream::SM_WRITE) return -1;
	if (m_pos > m_size) return 0;
	if (!dest || !m_buffer) return 0;

	if ((m_size - m_pos) < nr) nr = m_size - m_pos;

	char* cp = (char*)dest;
	char* op = (char*)&m_buffer[m_pos];
	fbtMemcpy(cp, op, nr);
	m_pos += nr;
	return nr;
}



FBTsize fbtMemoryStream::write(const void* src, FBTsize nr)
{
	if (m_mode == fbtStream::SM_READ || !src)
		return -1;

	if (m_pos > m_size) return 0;

	if (m_buffer == 0)
		reserve(m_pos + (nr));
	else if (m_pos + nr > m_capacity)
		reserve(m_pos + (nr > 65535 ? nr : nr + 65535));

	char* cp = &m_buffer[m_pos];
	fbtMemcpy(cp, (char*)src, nr);

	m_pos   += nr;
	m_size  += nr;
	return nr;
}



FBTsize fbtMemoryStream::writef(const char* fmt, ...)
{
	static char tmp[1024];

	va_list lst;
	va_start(lst, fmt);
	int size = fbtp_printf(tmp, 1024, fmt, lst);
	va_end(lst);


	if (size > 0)
	{
		tmp[size] = 0;
		return write(tmp, size);
	}

	return -1;

}
void fbtMemoryStream::reserve(FBTsize nr)
{
	if (m_capacity < nr)
	{
		char* buf = new char[nr + 1];
		if (m_buffer != 0)
		{
			fbtMemcpy(buf, m_buffer, m_size);
			delete [] m_buffer;
		}

		m_buffer = buf;
		m_buffer[m_size] = 0;
		m_capacity = nr;
	}
}
void fbtMemoryStream::shrinkToFit()
{
	if (m_capacity > m_size+1)
	{
		char* buf = new char[m_size + 1];
		if (m_buffer != 0)
		{
			fbtMemcpy(buf, m_buffer, m_size);
			delete [] m_buffer;
		}

		m_buffer = buf;
		m_buffer[m_size] = 0;
		m_capacity = m_size;
	}
} 


// Source File "fbtTables.cpp"
//#define FBT_IN_SOURCE
//#include "fbtTables.h"
//#include "fbtPlatformHeaders.h"


FBTsizeType fbtStruct::getUnlinkedMemberCount()
{
	FBTsizeType count = 0;
	for (FBTsizeType i = 0; i < m_members.size(); i++)
		if (!m_members[i].m_link) count++;

	return count;
}


fbtBinTables::fbtBinTables()
	:   m_name(0),
	    m_type(0),
	    m_tlen(0),
	    m_strc(0),
	    m_nameNr(0),
	    m_typeNr(0),
	    m_strcNr(0),
	    m_ptr(FBT_VOID),
	    m_otherBlock(0),
	    m_otherLen(0)
{
}

fbtBinTables::fbtBinTables(void* ptr, const FBTsize& len)
	:   m_name(0),
	    m_type(0),
	    m_tlen(0),
	    m_strc(0),
	    m_nameNr(0),
	    m_typeNr(0),
	    m_strcNr(0),
	    m_ptr(FBT_VOID),
	    m_otherBlock(ptr),
	    m_otherLen(len)
{
}

fbtBinTables::~fbtBinTables()
{
	fbtFree(m_name);
	fbtFree(m_type);
	fbtFree(m_tlen);
	fbtFree(m_strc);
	if (m_otherBlock)
		fbtFree(m_otherBlock);


    OffsM::Iterator it = m_offs.iterator();
	while (it.hasMoreElements())
		delete it.getNext();
}


bool fbtBinTables::read(bool swap)
{
	if (m_otherBlock && m_otherLen != 0)
		return read(m_otherBlock, m_otherLen, swap);
	return false;
}


bool fbtBinTables::read(const void* ptr, const FBTsize& /*len*/, bool swap)
{
	FBTuint32* ip = 0, i, j, k, nl;
	FBTtype* tp = 0;

	char* cp = (char*)ptr;


    //if (!fbtCharNEq(cp,fbtIdNames::FBT_SDNA, FBT_MAGIC))  // -Waddress
    if (!(cp && !strncmp(cp,fbtIdNames::FBT_SDNA, FBT_MAGIC)))
	{
		fbtPrintf("Bin table is missing the start id!\n");
		return false;
	}

	cp += FBT_MAGIC;


    //if (!fbtCharNEq(cp, fbtIdNames::FBT_NAME, FBT_MAGIC))
    if (!(cp && !strncmp(cp, fbtIdNames::FBT_NAME, FBT_MAGIC)))
	{
		fbtPrintf("Bin table is missing the name id!\n");
		return false;
	}

	cp += FBT_MAGIC;


	FBTintPtr opad;

	ip = (FBTuint32*)cp;
	nl = *ip++;
	cp = (char*)ip;

	if (swap) nl = fbtSwap32(nl);


	if (nl > fbtMaxTable)
	{
		fbtPrintf("Max name table size exceeded!\n");
		return false;
	}
	else
	{
		m_name = (Names)fbtMalloc((nl * sizeof(fbtName)) + 1);
	}



	i = 0;
	while (i < nl && i < fbtMaxTable)
	{
        fbtName name = {cp,(int) i, fbtCharHashKey(cp).hash(), 0, 0, 0, 1, {0}};

		fbtFixedString<64> bn;

		// re-lex
		while (*cp)
		{
			switch (*cp)
			{
			default:
				{
					bn.push_back(*cp);
					++cp; break;
				}
			case ')':
			case ']':
				++cp;
				break;
			case '(':   {++cp; name.m_isFptr = 1; break;    }
			case '*':   {++cp; name.m_ptrCount ++; break;   }
			case '[':
				{
					while ((*++cp) != ']')
						name.m_slots[name.m_numSlots] = (name.m_slots[name.m_numSlots] * 10) + ((*cp) - '0');
					name.m_arraySize *= name.m_slots[name.m_numSlots++];
				}
				break;
			}
		}
		++cp;

		//fbtPrintf("%d %d: %s %s %u %u\n", m_nameNr, m_base.size(), name.m_name, bn.c_str(), name.m_nameId, bn.hash());

		m_name[m_nameNr++] = name;
		m_base.push_back(bn.hash());
		++i;
	}

	// read alignment
	opad = (FBTintPtr)cp;
	opad = ((opad + 3) & ~3) - opad;
	while (opad--) cp++;

    //if (!fbtCharNEq(cp, fbtIdNames::FBT_TYPE, FBT_MAGIC))
    if (!(cp && !strncmp(cp, fbtIdNames::FBT_TYPE, FBT_MAGIC)))
	{
		fbtPrintf("Bin table is missing the type id!\n");
		return false;
	}

	cp += FBT_MAGIC;

	ip = (FBTuint32*)cp;
	nl = *ip++;
	cp = (char*)ip;

	if (swap) nl = fbtSwap32(nl);

	if (nl > fbtMaxTable)
	{
		fbtPrintf("Max name table size exceeded!\n");
		return false;
	}
	else
	{
		m_type = (Types)fbtMalloc((nl * sizeof(fbtType) + 1));
		m_tlen = (TypeL)fbtMalloc((nl * sizeof(FBTtype) + 1));
	}

	i = 0;
	while (i < nl)
	{
        fbtType typeData = {cp, fbtCharHashKey(cp).hash(), (FBTuint32)-1};  // Added explicit cast
		m_type[m_typeNr++] = typeData;
		while (*cp) ++cp;
		++cp;
		++i;
	}

	// read alignment
	opad = (FBTintPtr)cp;
	opad = ((opad + 3) & ~3) - opad;
	while (opad--) cp++;

    //if (!fbtCharNEq(cp, fbtIdNames::FBT_TLEN, FBT_MAGIC))
    if (!(cp && !strncmp(cp, fbtIdNames::FBT_TLEN, FBT_MAGIC)))
	{
		fbtPrintf("Bin table is missing the tlen id!\n");
		return false;
	}

	cp += FBT_MAGIC;

	tp = (FBTtype*)cp;

	i = 0;
	while (i < m_typeNr)
	{

		m_tlen[i] = *tp++;
		if (swap)
			m_tlen[i] = fbtSwap16(m_tlen[i]);
		++i;
	}

	// read alignment
	if (m_typeNr & 1) ++tp;

	cp = (char*)tp;

    //if (!fbtCharNEq(cp, fbtIdNames::FBT_STRC, FBT_MAGIC))
    if (!(cp && !strncmp(cp, fbtIdNames::FBT_STRC, FBT_MAGIC)))
	{
		fbtPrintf("Bin table is missing the tlen id!\n");
		return false;
	}

	cp += FBT_MAGIC;


	ip = (FBTuint32*)cp;
	nl = *ip++;
	tp = (FBTtype*)ip;

	if (swap) nl = fbtSwap32(nl);

	if (nl > fbtMaxTable)
	{
		fbtPrintf("Max name table size exceeded!\n");
		return false;
	}
	else
		m_strc = (Strcs)fbtMalloc(nl * fbtMaxMember * sizeof(FBTtype) + 1);


	m_typeFinder.reserve(m_typeNr);


	i = 0;
	while (i < nl)
	{
		m_strc[m_strcNr++] = tp;


		if (swap)
		{
			tp[0] = fbtSwap16(tp[0]);
			tp[1] = fbtSwap16(tp[1]);

			m_type[tp[0]].m_strcId = m_strcNr - 1;

			m_typeFinder.insert(m_type[tp[0]].m_name, m_type[tp[0]]);


			k = tp[1];
			FBT_ASSERT( k < fbtMaxMember );

			j = 0;
			tp += 2;

			while (j < k)
			{
				tp[0] = fbtSwap16(tp[0]);
				tp[1] = fbtSwap16(tp[1]);

				++j;
				tp += 2;
			}
		}
		else
		{
			FBT_ASSERT( tp[1] < fbtMaxMember );
			m_type[tp[0]].m_strcId = m_strcNr - 1;
			m_typeFinder.insert(m_type[tp[0]].m_name, m_type[tp[0]]);

			tp += (2 * tp[1]) + 2;
		}

		++i;
	}

	if (m_strcNr == 0)
	{
		fbtFree(m_name);
		fbtFree(m_type);
		fbtFree(m_tlen);
		fbtFree(m_strc);

		m_name = 0;
		m_type = 0;
		m_tlen = 0;
		m_strc = 0;

		return false;
	}

	compile();
	return true;
}


void fbtBinTables::compile(FBTtype i, FBTtype nr, fbtStruct* off, FBTuint32& cof, FBTuint32 depth, fbtStruct::Keys& keys)
{
	FBTuint32 e, l, a, oof, ol;
	FBTuint16 f = m_strc[0][0];

	if (i > m_strcNr)
	{
		fbtPrintf("Missing recursive type\n");
		return;
	}


	for (a = 0; a < nr; ++a)
	{
		// Only calculate offsets on recursive structs
		// This saves undeded buffers
		FBTtype* strc = m_strc[i];

		oof = cof;
		ol = m_tlen[strc[0]];

		l = strc[1];
		strc += 2;

		for (e = 0; e < l; e++, strc += 2)
		{
			if (strc[0] >= f && m_name[strc[1]].m_ptrCount == 0)
			{
                fbtKey64 k = {{m_type[strc[0]].m_typeId, m_name[strc[1]].m_nameId}};
				keys.push_back(k);

				compile(m_type[strc[0]].m_strcId, m_name[strc[1]].m_arraySize, off, cof, depth+1, keys);

				keys.pop_back();
			}
			else
				putMember(strc, off, a, cof, depth, keys);
		}

		if ((cof - oof) != ol)
			fbtPrintf("Build ==> invalid offset (%i)(%i:%i)\n", a, (cof - oof), ol);
	
	}
}




void fbtBinTables::compile(void)
{
	m_offs.reserve(fbtMaxTable);

	if (!m_strc || m_strcNr <= 0)
	{
		fbtPrintf("Build ==> No structurs.");
		return;
	}

	FBTuint32 i, cof = 0, depth;
	FBTuint16 f = m_strc[0][0], e, memberCount;

	fbtStruct::Keys emptyKeys;
	for (i = 0; i < m_strcNr; i++)
	{
		FBTtype* strc = m_strc[i];

		FBTtype strcType = strc[0];

		depth = 0;
		cof = 0;
		fbtStruct* off = new fbtStruct;
		off->m_key.k16[0] = strcType;
		off->m_key.k16[1] = 0;
		off->m_val.k32[0] = m_type[strcType].m_typeId;
		off->m_val.k32[1] = 0; // no name
		off->m_nr         = 0;
		off->m_dp         = 0;
		off->m_off        = cof;
		off->m_len        = m_tlen[strcType];
		off->m_strcId     = i;
		off->m_link       = 0;
		off->m_flag       = fbtStruct::CAN_LINK;

		m_offs.push_back(off);

		memberCount = strc[1];

		strc += 2;
		off->m_members.reserve(fbtMaxMember);

		for (e = 0; e < memberCount; ++e, strc += 2)
		{
			if (strc[0] >= f && m_name[strc[1]].m_ptrCount == 0) //strc[0]:member_type, strc[1]:member_name
			{
				fbtStruct::Keys keys;
                fbtKey64 k = {{m_type[strc[0]].m_typeId, m_name[strc[1]].m_nameId}};
				keys.push_back(k);
				compile(m_type[strc[0]].m_strcId, m_name[strc[1]].m_arraySize, off, cof, depth+1, keys);				
			}
			else
				putMember(strc, off, 0, cof, 0, emptyKeys);
		}

        if ((int)cof != (int)off->m_len)
		{
			off->m_flag |= fbtStruct::MISALIGNED;
			fbtPrintf("Build ==> invalid offset %s:%i:%i:%i\n", m_type[off->m_key.k16[0]].m_name, i, cof, off->m_len);
		}

	}
}

void fbtBinTables::putMember(FBTtype* cp, fbtStruct* off, FBTtype nr, FBTuint32& cof, FBTuint32 depth, fbtStruct::Keys& keys)
{
	fbtStruct nof;
	nof.m_key.k16[0] = cp[0];
	nof.m_key.k16[1] = cp[1];
	nof.m_val.k32[0] = m_type[cp[0]].m_typeId;
	nof.m_val.k32[1] = m_base[cp[1]];
	nof.m_off        = cof;
	nof.m_strcId     = off->m_strcId;
	nof.m_nr         = nr;
	nof.m_dp         = depth;
	nof.m_link       = 0;
	nof.m_flag       = fbtStruct::CAN_LINK;
	nof.m_len        = (m_name[cp[1]].m_ptrCount ? m_ptr : m_tlen[cp[0]]) * m_name[cp[1]].m_arraySize;
	nof.m_keyChain   = keys;
	off->m_members.push_back(nof);
	cof += nof.m_len;

#ifdef _DEBUG
	//fbtPrintf("%s %s\n", getStructType(off), getStructName(off));
	//fbtPrintf("\t%s %s nr:%d cof:%d depth:%d\n", getStructType(&nof), getStructName(&nof), nr, cof, depth);
#endif
}


FBTtype fbtBinTables::findTypeId(const fbtCharHashKey &cp)
{
	FBTsizeType pos = m_typeFinder.find(cp);
	if (pos != FBT_NPOS)
		return m_typeFinder.at(pos).m_strcId;
	return -1;
}

const char* fbtBinTables::getStructType(const fbtStruct* strc)
{
	
	//return strc ? m_type[strc->m_key.k16[0]].m_name : "";

	FBTuint32 k = strc ? strc->m_key.k16[0] : (FBTuint32)-1;	
	return  (k >= m_typeNr) ? "" : m_type[k].m_name;
}

const char* fbtBinTables::getStructName(const fbtStruct* strc)
{	
	FBTuint32 k = strc ? strc->m_key.k16[1] : (FBTuint32)-1;	
	return  (k >= m_nameNr) ? "" : m_name[k].m_name;
}

const char* fbtBinTables::getOwnerStructName(const fbtStruct* strc)
{
	//cp0 = mp->m_type[mp->m_strc[c->m_strcId][0]].m_name;
	FBTuint32 k = strc ? strc->m_strcId : (FBTuint32)-1;
	return (k >= m_strcNr || *m_strc[k] >= m_typeNr) ? "" : m_type[*m_strc[k]].m_name;
}


// Source File "fbtTypes.cpp"
//#define FBT_IN_SOURCE
//#include "fbtPlatformHeaders.h"


// ----------------------------------------------------------------------------
// Debug Utilities


#ifdef FBT_DEBUG
bool fbtDebugger::isDebugger(void)
{
#if FBT_COMPILER == FBT_COMPILER_MSVC && _WIN32_WINNT >= 0x0400 && _MSC_VER>=1400
    return IsDebuggerPresent() != 0;
#else
	return false;
#endif
}

void fbtDebugger::breakProcess(void)
{
#if FBT_COMPILER == FBT_COMPILER_MSVC
	_asm int 3;
#else
	asm("int $3");
#endif
}

#else//FBT_DEBUG


bool fbtDebugger::isDebugger(void)
{
	return false;
}

void fbtDebugger::breakProcess(void)
{
}

#endif//FBT_DEBUG


#define FBT_DEBUG_BUF_SIZE (1024)
fbtDebugger::Reporter fbtDebugger::m_report = {0, 0};


void fbtDebugger::setReportHook(Reporter& hook)
{
	m_report.m_client = hook.m_client;
	m_report.m_hook   = hook.m_hook;
}


void fbtDebugger::report(const char* fmt, ...)
{
	char ReportBuf[FBT_DEBUG_BUF_SIZE+1];

	va_list lst;
	va_start(lst, fmt);
	int size = fbtp_printf(ReportBuf, FBT_DEBUG_BUF_SIZE, fmt, lst);
	va_end(lst);

	if (size < 0)
	{
		ReportBuf[FBT_DEBUG_BUF_SIZE] = 0;
		size = FBT_DEBUG_BUF_SIZE;
	}

	if (size > 0)
	{
		ReportBuf[size] = 0;

		if (m_report.m_hook)
		{
#if FBT_COMPILER == FBT_COMPILER_MSVC && _WIN32_WINNT >= 0x0400 && _MSC_VER>=1400
			if (IsDebuggerPresent())
				OutputDebugString(ReportBuf);

#endif
			m_report.m_hook(m_report.m_client, ReportBuf);
		}
		else
		{

#if FBT_COMPILER == FBT_COMPILER_MSVC && _WIN32_WINNT >= 0x0400 && _MSC_VER>=1400
			if (IsDebuggerPresent())
				OutputDebugString(ReportBuf);
			else
#endif
				fprintf(stderr, "%s", ReportBuf);
		}
	}

}


void fbtDebugger::reportIDE(const char* src, long line, const char* fmt, ...)
{
	static char ReportBuf[FBT_DEBUG_BUF_SIZE+1];

	va_list lst;
	va_start(lst, fmt);


	int size = fbtp_printf(ReportBuf, FBT_DEBUG_BUF_SIZE, fmt, lst);
	va_end(lst);

	if (size < 0)
	{
		ReportBuf[FBT_DEBUG_BUF_SIZE] = 0;
		size = FBT_DEBUG_BUF_SIZE;
	}

	if (size > 0)
	{
		ReportBuf[size] = 0;
#if FBT_COMPILER == FBT_COMPILER_MSVC
		report("%s(%i): warning: %s", src, line, ReportBuf);
#else
		report("%s:%i: warning: %s", src, line, ReportBuf);
#endif
	}
}


void fbtDebugger::errorIDE(const char* src, long line, const char* fmt, ...)
{
	static char ReportBuf[FBT_DEBUG_BUF_SIZE+1];

	va_list lst;
	va_start(lst, fmt);


	int size = fbtp_printf(ReportBuf, FBT_DEBUG_BUF_SIZE, fmt, lst);
	va_end(lst);

	if (size < 0)
	{
		ReportBuf[FBT_DEBUG_BUF_SIZE] = 0;
		size = FBT_DEBUG_BUF_SIZE;
	}

	if (size > 0)
	{
		ReportBuf[size] = 0;
#if FBT_COMPILER == FBT_COMPILER_MSVC
		report("%s(%i): error: %s", src, line, ReportBuf);
#else
		report("%s:%i: error: %s", src, line, ReportBuf);
#endif
	}
}

FBT_PRIM_TYPE fbtGetPrimType(FBTuint32 typeKey)
{
	static FBTuint32 charT    = fbtCharHashKey("char").hash();
	static FBTuint32 ucharT   = fbtCharHashKey("uchar").hash();
	static FBTuint32 shortT   = fbtCharHashKey("short").hash();
	static FBTuint32 ushortT  = fbtCharHashKey("ushort").hash();
	static FBTuint32 intT     = fbtCharHashKey("int").hash();
	static FBTuint32 longT    = fbtCharHashKey("long").hash();
	static FBTuint32 ulongT   = fbtCharHashKey("ulong").hash();
	static FBTuint32 floatT   = fbtCharHashKey("float").hash();
	static FBTuint32 doubleT  = fbtCharHashKey("double").hash();
	static FBTuint32 voidT    = fbtCharHashKey("void").hash();

	if (typeKey == charT)	return FBT_PRIM_CHAR;
	if (typeKey == ucharT)	return FBT_PRIM_UCHAR;
	if (typeKey == shortT)	return FBT_PRIM_SHORT;
	if (typeKey == ushortT)	return FBT_PRIM_USHORT;
	if (typeKey == intT)	return FBT_PRIM_INT;
	if (typeKey == longT)	return FBT_PRIM_LONG;
	if (typeKey == ulongT)	return FBT_PRIM_ULONG;
	if (typeKey == floatT)	return FBT_PRIM_FLOAT;
	if (typeKey == doubleT)	return FBT_PRIM_DOUBLE;
	if (typeKey == voidT)	return FBT_PRIM_VOID;

	return FBT_PRIM_UNKNOWN;
}


#endif //_fbtFile_imp_



const FBTuint32 GLOB = FBT_ID('G', 'L', 'O', 'B');

struct fbtIdDB
{
	const FBTuint16     m_code;
	fbtList             fbtBlend::*m_ptr;
};

// Warning: these globals are NOT updated automatically, but must be MANUALLY integrated (to reflect the content of source/blender/makesdna/DNA_ID_enums.h)
// It should be OK to use "integrated versions" also for older versions (newer fields are not present in their Blender DNA, and this should be OK). [DNA_ID_enums 2/2]
fbtIdDB fbtData[] =
{
	{ FBT_ID2('S', 'C'), &fbtBlend::m_scene},
	{ FBT_ID2('L', 'I'), &fbtBlend::m_library},
	{ FBT_ID2('O', 'B'), &fbtBlend::m_object},
	{ FBT_ID2('M', 'E'), &fbtBlend::m_mesh},
	{ FBT_ID2('C', 'U'), &fbtBlend::m_curve},	// ID_CV should be used in the future (see #95355).
	{ FBT_ID2('M', 'B'), &fbtBlend::m_mball},
	{ FBT_ID2('M', 'A'), &fbtBlend::m_mat},
	{ FBT_ID2('T', 'E'), &fbtBlend::m_tex},
	{ FBT_ID2('I', 'M'), &fbtBlend::m_image},
	{ FBT_ID2('L', 'T'), &fbtBlend::m_latt},
	{ FBT_ID2('L', 'A'), &fbtBlend::m_lamp},
	{ FBT_ID2('C', 'A'), &fbtBlend::m_camera},
	{ FBT_ID2('I', 'P'), &fbtBlend::m_ipo},
	{ FBT_ID2('K', 'E'), &fbtBlend::m_key},
	{ FBT_ID2('W', 'O'), &fbtBlend::m_world},
#	if BLENDER_VERSION<306	// not sure when the switch was made...
	{ FBT_ID2('S', 'N'), &fbtBlend::m_screen},
#	else
	{ FBT_ID2('S', 'R'), &fbtBlend::m_screen},
#	endif	
	{ FBT_ID2('P', 'Y'), &fbtBlend::m_script},
	{ FBT_ID2('V', 'F'), &fbtBlend::m_vfont},
	{ FBT_ID2('T', 'X'), &fbtBlend::m_text},
	{ FBT_ID2('S', 'K'), &fbtBlend::m_speaker},
	{ FBT_ID2('S', 'O'), &fbtBlend::m_sound},
	{ FBT_ID2('G', 'R'), &fbtBlend::m_group},
	{ FBT_ID2('A', 'R'), &fbtBlend::m_armature},
	{ FBT_ID2('A', 'C'), &fbtBlend::m_action},
	{ FBT_ID2('N', 'T'), &fbtBlend::m_nodetree},
	{ FBT_ID2('B', 'R'), &fbtBlend::m_brush},
	{ FBT_ID2('P', 'A'), &fbtBlend::m_particle},
	{ FBT_ID2('G', 'D'), &fbtBlend::m_gpencilLegacy},
	{ FBT_ID2('W', 'M'), &fbtBlend::m_wm},
	{ FBT_ID2('M', 'C'), &fbtBlend::m_movieClip},
	{ FBT_ID2('M', 'S'), &fbtBlend::m_mask},
	{ FBT_ID2('L', 'S'), &fbtBlend::m_freeStyleLineStyle},
	{ FBT_ID2('P', 'L'), &fbtBlend::m_palette},
	{ FBT_ID2('P', 'C'), &fbtBlend::m_paintCurve},
	{ FBT_ID2('C', 'F'), &fbtBlend::m_cacheFile},
	{ FBT_ID2('W', 'S'), &fbtBlend::m_workSpace},
	{ FBT_ID2('L', 'P'), &fbtBlend::m_lightProbe},
	{ FBT_ID2('H', 'A'), &fbtBlend::m_hair},
	{ FBT_ID2('C', 'V'), &fbtBlend::m_curves},
	{ FBT_ID2('P', 'T'), &fbtBlend::m_pointCloud},
	{ FBT_ID2('V', 'O'), &fbtBlend::m_volume},
	{ FBT_ID2('S', 'I'), &fbtBlend::m_simulation},
	{ FBT_ID2('G', 'P'), &fbtBlend::m_gpencil},
	{ 0, 0 }
};




extern unsigned char bfBlenderFBT[];
extern int bfBlenderLen;


fbtBlend::fbtBlend()
	:   fbtFile("BLENDER"), m_stripList(0)
{
	m_aluhid = "BLENDEs"; //a stripped blend file
}



fbtBlend::~fbtBlend()
{
}



int fbtBlend::initializeTables(fbtBinTables* tables)
{
	return tables->read(bfBlenderFBT, bfBlenderLen, false) ? FS_OK : FS_FAILED;
}



int fbtBlend::notifyData(void* p, const Chunk& id)
{
	if (id.m_code == GLOB)
	{
		m_fg = (Blender::FileGlobal*)p;
		return FS_OK;
	}

	if ((id.m_code <= 0xFFFF))
	{
		int i = 0;
		while (fbtData[i].m_code != 0)
		{
			if (fbtData[i].m_code == id.m_code)
			{
				(this->*fbtData[i].m_ptr).push_back(p);
				break;
			}
			++i;
		}
	}
	return FS_OK;
}


int fbtBlend::writeData(fbtStream* stream)
{
	fbtBinTables::OffsM::Pointer md = m_memory->m_offs.ptr();


	for (MemoryChunk* node = (MemoryChunk*)m_chunks.first; node; node = node->m_next)
	{
		if (node->m_newTypeId > m_memory->m_strcNr)
			continue;
		if (!node->m_newBlock)
			continue;

		void* wd = node->m_newBlock;

		Chunk ch;
		ch.m_code   = node->m_chunk.m_code;
		ch.m_nr     = node->m_chunk.m_nr;
		ch.m_len    = node->m_chunk.m_len;
		ch.m_typeid = node->m_newTypeId;
		ch.m_old    = (FBTsize)wd;

		stream->write(&ch, sizeof(Chunk));
		stream->write(wd, ch.m_len);
	}

	return FS_OK;
}



bool fbtBlend::skip(const FBTuint32& id)
{

	if (!m_stripList)
		return false;

	int i = 0;
	while (m_stripList[i] != 0)
	{
		if (m_stripList[i++] == id)
			return true;
	}

	return false;
}


void*   fbtBlend::getFBT(void)
{
	return (void*)bfBlenderFBT;
}

FBTsize fbtBlend::getFBTlength(void)
{
	return bfBlenderLen;
}

int fbtBlend::save(const char *path, const int mode)
{
	m_version = m_fileVersion;
	return reflect(path, mode);
}


// bfBlender.cpp: THIS DEPENDS ON THE BLENDER VERSION! =========================================

// Generated using BLENDER-v405
unsigned char bfBlenderFBT[] = {
83,68,78,65,78,65,77,69,151,20,0,0,42,100,101,115,99,114,105,112,116,105,111,110,0,114,110,97,95,115,117,98,116,121,112,101,0,95,112,97,100,91,52,93,0,42,105,100,101,110,116,105,102,105,101,114,0,42,110,97,109,101,0,118,97,108,117,101,0,105,99,111,110,0,98,97,115,101,0,42,100,101,102,97,117,108,116,95,97,114,114,97,121,0,100,101,102,97,117,108,116,95,97,114,114,97,121,95,108,101,110,0,109,105,110,0,109,97,120,0,
115,111,102,116,95,109,105,110,0,115,111,102,116,95,109,97,120,0,115,116,101,112,0,100,101,102,97,117,108,116,95,118,97,108,117,101,0,101,110,117,109,95,105,116,101,109,115,95,110,117,109,0,42,101,110,117,109,95,105,116,101,109,115,0,95,112,97,100,91,51,93,0,112,114,101,99,105,115,105,111,110,0,42,100,101,102,97,117,108,116,95,118,97,108,117,101,0,105,100,95,116,121,112,101,0,95,112,97,100,91,54,93,0,42,112,111,105,110,116,101,
114,0,103,114,111,117,112,0,118,97,108,0,118,97,108,50,0,42,110,101,120,116,0,42,112,114,101,118,0,116,121,112,101,0,115,117,98,116,121,112,101,0,102,108,97,103,0,110,97,109,101,91,54,52,93,0,95,112,97,100,48,91,52,93,0,100,97,116,97,0,108,101,110,0,116,111,116,97,108,108,101,110,0,42,117,105,95,100,97,116,97,0,111,112,101,114,97,116,105,111,110,0,116,97,103,0,95,112,97,100,48,91,50,93,0,42,115,117,98,105,
116,101,109,95,114,101,102,101,114,101,110,99,101,95,110,97,109,101,0,42,115,117,98,105,116,101,109,95,108,111,99,97,108,95,110,97,109,101,0,115,117,98,105,116,101,109,95,114,101,102,101,114,101,110,99,101,95,105,110,100,101,120,0,115,117,98,105,116,101,109,95,108,111,99,97,108,95,105,110,100,101,120,0,42,115,117,98,105,116,101,109,95,114,101,102,101,114,101,110,99,101,95,105,100,0,42,115,117,98,105,116,101,109,95,108,111,99,97,108,95,
105,100,0,42,114,110,97,95,112,97,116,104,0,111,112,101,114,97,116,105,111,110,115,0,95,112,97,100,91,50,93,0,114,110,97,95,112,114,111,112,95,116,121,112,101,0,42,114,101,102,101,114,101,110,99,101,0,112,114,111,112,101,114,116,105,101,115,0,42,104,105,101,114,97,114,99,104,121,95,114,111,111,116,0,42,114,117,110,116,105,109,101,0,95,112,97,100,95,49,91,52,93,0,115,116,97,116,117,115,0,115,107,105,112,112,101,100,95,114,101,
102,99,111,117,110,116,101,100,0,115,107,105,112,112,101,100,95,100,105,114,101,99,116,0,115,107,105,112,112,101,100,95,105,110,100,105,114,101,99,116,0,114,101,109,97,112,0,42,100,101,112,115,103,114,97,112,104,0,42,114,101,97,100,102,105,108,101,95,100,97,116,97,0,42,110,101,119,105,100,0,42,108,105,98,0,42,97,115,115,101,116,95,100,97,116,97,0,110,97,109,101,91,54,54,93,0,117,115,0,105,99,111,110,95,105,100,0,114,101,99,
97,108,99,0,114,101,99,97,108,99,95,117,112,95,116,111,95,117,110,100,111,95,112,117,115,104,0,114,101,99,97,108,99,95,97,102,116,101,114,95,117,110,100,111,95,112,117,115,104,0,115,101,115,115,105,111,110,95,117,105,100,0,42,112,114,111,112,101,114,116,105,101,115,0,42,115,121,115,116,101,109,95,112,114,111,112,101,114,116,105,101,115,0,42,95,112,97,100,49,0,42,111,118,101,114,114,105,100,101,95,108,105,98,114,97,114,121,0,42,111,
114,105,103,95,105,100,0,42,112,121,95,105,110,115,116,97,110,99,101,0,42,108,105,98,114,97,114,121,95,119,101,97,107,95,114,101,102,101,114,101,110,99,101,0,114,117,110,116,105,109,101,0,105,100,0,110,97,109,101,91,49,48,50,52,93,0,42,112,97,99,107,101,100,102,105,108,101,0,108,105,98,114,97,114,121,95,102,105,108,101,112,97,116,104,91,49,48,50,52,93,0,108,105,98,114,97,114,121,95,105,100,95,110,97,109,101,91,54,54,93,
0,119,91,50,93,0,104,91,50,93,0,102,108,97,103,91,50,93,0,99,104,97,110,103,101,100,95,116,105,109,101,115,116,97,109,112,91,50,93,0,42,114,101,99,116,91,50,93,0,99,111,91,51,93,0,42,112,111,105,110,116,115,0,108,101,110,103,116,104,0,115,116,97,114,116,95,102,114,97,109,101,0,101,110,100,95,102,114,97,109,101,0,99,111,108,111,114,91,51,93,0,99,111,108,111,114,95,112,111,115,116,91,51,93,0,108,105,110,101,95,
116,104,105,99,107,110,101,115,115,0,95,112,97,100,50,91,52,93,0,42,112,111,105,110,116,115,95,118,98,111,0,42,98,97,116,99,104,95,108,105,110,101,0,42,98,97,116,99,104,95,112,111,105,110,116,115,0,42,95,112,97,100,0,112,97,116,104,95,116,121,112,101,0,112,97,116,104,95,115,116,101,112,0,112,97,116,104,95,114,97,110,103,101,0,112,97,116,104,95,118,105,101,119,102,108,97,103,0,112,97,116,104,95,98,97,107,101,102,108,97,
103,0,112,97,116,104,95,115,102,0,112,97,116,104,95,101,102,0,112,97,116,104,95,98,99,0,112,97,116,104,95,97,99,0,112,111,105,110,116,91,51,93,0,112,108,97,110,101,95,110,111,114,109,97,108,91,51,93,0,112,108,97,110,101,95,111,102,102,115,101,116,0,100,101,112,116,104,95,115,99,97,108,101,0,100,101,102,111,114,109,95,100,117,97,108,95,113,117,97,116,0,98,98,111,110,101,95,115,101,103,109,101,110,116,115,0,98,98,111,110,
101,95,97,114,99,95,108,101,110,103,116,104,95,114,101,99,105,112,114,111,99,97,108,0,95,112,97,100,49,91,52,93,0,42,98,98,111,110,101,95,114,101,115,116,95,109,97,116,115,0,42,98,98,111,110,101,95,112,111,115,101,95,109,97,116,115,0,42,98,98,111,110,101,95,100,101,102,111,114,109,95,109,97,116,115,0,42,98,98,111,110,101,95,100,117,97,108,95,113,117,97,116,115,0,42,98,98,111,110,101,95,115,101,103,109,101,110,116,95,98,
111,117,110,100,97,114,105,101,115,0,42,112,114,111,112,0,99,111,110,115,116,114,97,105,110,116,115,0,105,107,102,108,97,103,0,112,114,111,116,101,99,116,102,108,97,103,0,97,103,114,112,95,105,110,100,101,120,0,99,111,110,115,116,102,108,97,103,0,115,101,108,101,99,116,102,108,97,103,0,100,114,97,119,102,108,97,103,0,98,98,111,110,101,102,108,97,103,0,42,98,111,110,101,0,42,112,97,114,101,110,116,0,42,99,104,105,108,100,0,105,
107,116,114,101,101,0,115,105,107,116,114,101,101,0,42,109,112,97,116,104,0,42,99,117,115,116,111,109,0,42,99,117,115,116,111,109,95,116,120,0,99,117,115,116,111,109,95,115,99,97,108,101,0,99,117,115,116,111,109,95,115,99,97,108,101,95,120,121,122,91,51,93,0,99,117,115,116,111,109,95,116,114,97,110,115,108,97,116,105,111,110,91,51,93,0,99,117,115,116,111,109,95,114,111,116,97,116,105,111,110,95,101,117,108,101,114,91,51,93,0,
99,117,115,116,111,109,95,115,104,97,112,101,95,119,105,114,101,95,119,105,100,116,104,0,108,111,99,91,51,93,0,115,105,122,101,91,51,93,0,101,117,108,91,51,93,0,113,117,97,116,91,52,93,0,114,111,116,65,120,105,115,91,51,93,0,114,111,116,65,110,103,108,101,0,114,111,116,109,111,100,101,0,99,104,97,110,95,109,97,116,91,52,93,91,52,93,0,112,111,115,101,95,109,97,116,91,52,93,91,52,93,0,100,105,115,112,95,109,97,116,
91,52,93,91,52,93,0,100,105,115,112,95,116,97,105,108,95,109,97,116,91,52,93,91,52,93,0,99,111,110,115,116,105,110,118,91,52,93,91,52,93,0,112,111,115,101,95,104,101,97,100,91,51,93,0,112,111,115,101,95,116,97,105,108,91,51,93,0,108,105,109,105,116,109,105,110,91,51,93,0,108,105,109,105,116,109,97,120,91,51,93,0,115,116,105,102,102,110,101,115,115,91,51,93,0,105,107,115,116,114,101,116,99,104,0,105,107,114,111,116,
119,101,105,103,104,116,0,105,107,108,105,110,119,101,105,103,104,116,0,114,111,108,108,49,0,114,111,108,108,50,0,99,117,114,118,101,73,110,88,0,99,117,114,118,101,73,110,89,0,99,117,114,118,101,79,117,116,88,0,99,117,114,118,101,79,117,116,89,0,101,97,115,101,49,0,101,97,115,101,50,0,115,99,97,108,101,73,110,0,115,99,97,108,101,95,105,110,95,121,0,115,99,97,108,101,79,117,116,0,115,99,97,108,101,95,111,117,116,95,121,
0,115,99,97,108,101,95,105,110,91,51,93,0,115,99,97,108,101,95,111,117,116,91,51,93,0,42,98,98,111,110,101,95,112,114,101,118,0,42,98,98,111,110,101,95,110,101,120,116,0,42,116,101,109,112,0,42,100,114,97,119,95,100,97,116,97,0,42,111,114,105,103,95,112,99,104,97,110,0,99,111,108,111,114,0,42,95,112,97,100,50,0,99,104,97,110,98,97,115,101,0,42,99,104,97,110,104,97,115,104,0,42,42,99,104,97,110,95,97,114,
114,97,121,0,99,116,105,109,101,0,115,116,114,105,100,101,95,111,102,102,115,101,116,91,51,93,0,99,121,99,108,105,99,95,111,102,102,115,101,116,91,51,93,0,97,103,114,111,117,112,115,0,97,99,116,105,118,101,95,103,114,111,117,112,0,105,107,115,111,108,118,101,114,0,42,105,107,100,97,116,97,0,42,105,107,112,97,114,97,109,0,97,118,115,0,110,117,109,105,116,101,114,0,110,117,109,115,116,101,112,0,109,105,110,115,116,101,112,0,109,
97,120,115,116,101,112,0,115,111,108,118,101,114,0,102,101,101,100,98,97,99,107,0,109,97,120,118,101,108,0,100,97,109,112,109,97,120,0,100,97,109,112,101,112,115,0,99,104,97,110,110,101,108,115,0,102,99,117,114,118,101,95,114,97,110,103,101,95,115,116,97,114,116,0,102,99,117,114,118,101,95,114,97,110,103,101,95,108,101,110,103,116,104,0,42,99,104,97,110,110,101,108,95,98,97,103,0,99,117,115,116,111,109,67,111,108,0,99,115,0,
42,42,108,97,121,101,114,95,97,114,114,97,121,0,108,97,121,101,114,95,97,114,114,97,121,95,110,117,109,0,108,97,121,101,114,95,97,99,116,105,118,101,95,105,110,100,101,120,0,42,42,115,108,111,116,95,97,114,114,97,121,0,115,108,111,116,95,97,114,114,97,121,95,110,117,109,0,108,97,115,116,95,115,108,111,116,95,104,97,110,100,108,101,0,42,42,115,116,114,105,112,95,107,101,121,102,114,97,109,101,95,100,97,116,97,95,97,114,114,97,
121,0,115,116,114,105,112,95,107,101,121,102,114,97,109,101,95,100,97,116,97,95,97,114,114,97,121,95,110,117,109,0,99,117,114,118,101,115,0,103,114,111,117,112,115,0,109,97,114,107,101,114,115,0,97,99,116,105,118,101,95,109,97,114,107,101,114,0,105,100,114,111,111,116,0,102,114,97,109,101,95,115,116,97,114,116,0,102,114,97,109,101,95,101,110,100,0,42,112,114,101,118,105,101,119,0,42,115,111,117,114,99,101,0,42,102,105,108,116,101,
114,95,103,114,112,0,115,101,97,114,99,104,115,116,114,91,54,52,93,0,102,105,108,116,101,114,102,108,97,103,0,102,105,108,116,101,114,102,108,97,103,50,0,114,101,110,97,109,101,73,110,100,101,120,0,95,112,97,100,48,91,55,93,0,114,101,103,105,111,110,98,97,115,101,0,115,112,97,99,101,116,121,112,101,0,108,105,110,107,95,102,108,97,103,0,95,112,97,100,48,91,54,93,0,118,50,100,0,42,97,99,116,105,111,110,0,97,99,116,105,
111,110,95,115,108,111,116,95,104,97,110,100,108,101,0,97,100,115,0,116,105,109,101,115,108,105,100,101,0,109,111,100,101,0,109,111,100,101,95,112,114,101,118,0,97,117,116,111,115,110,97,112,0,99,97,99,104,101,95,100,105,115,112,108,97,121,0,95,112,97,100,49,91,54,93,0,42,103,114,112,0,42,105,112,111,0,99,111,110,115,116,114,97,105,110,116,67,104,97,110,110,101,108,115,0,116,101,109,112,0,105,110,102,108,117,101,110,99,101,0,
108,97,121,101,114,95,102,108,97,103,115,0,108,97,121,101,114,95,109,105,120,95,109,111,100,101,0,42,42,115,116,114,105,112,95,97,114,114,97,121,0,115,116,114,105,112,95,97,114,114,97,121,95,110,117,109,0,105,100,116,121,112,101,0,104,97,110,100,108,101,0,115,108,111,116,95,102,108,97,103,115,0,95,112,97,100,49,91,55,93,0,115,116,114,105,112,95,116,121,112,101,0,95,112,97,100,48,91,51,93,0,100,97,116,97,95,105,110,100,101,
120,0,102,114,97,109,101,95,111,102,102,115,101,116,0,42,42,99,104,97,110,110,101,108,98,97,103,95,97,114,114,97,121,0,99,104,97,110,110,101,108,98,97,103,95,97,114,114,97,121,95,110,117,109,0,115,108,111,116,95,104,97,110,100,108,101,0,103,114,111,117,112,95,97,114,114,97,121,95,110,117,109,0,42,42,103,114,111,117,112,95,97,114,114,97,121,0,102,99,117,114,118,101,95,97,114,114,97,121,95,110,117,109,0,42,42,102,99,117,114,
118,101,95,97,114,114,97,121,0,42,99,117,114,118,101,0,42,100,97,116,97,0,117,105,95,101,120,112,97,110,100,95,102,108,97,103,0,115,102,114,97,0,101,102,114,97,0,98,108,101,110,100,105,110,0,98,108,101,110,100,111,117,116,0,42,99,111,101,102,102,105,99,105,101,110,116,115,0,97,114,114,97,121,115,105,122,101,0,112,111,108,121,95,111,114,100,101,114,0,97,109,112,108,105,116,117,100,101,0,112,104,97,115,101,95,109,117,108,116,105,
112,108,105,101,114,0,112,104,97,115,101,95,111,102,102,115,101,116,0,118,97,108,117,101,95,111,102,102,115,101,116,0,116,105,109,101,0,102,49,0,102,50,0,116,111,116,118,101,114,116,0,109,105,100,118,97,108,0,98,101,102,111,114,101,95,109,111,100,101,0,97,102,116,101,114,95,109,111,100,101,0,98,101,102,111,114,101,95,99,121,99,108,101,115,0,97,102,116,101,114,95,99,121,99,108,101,115,0,114,101,99,116,0,115,105,122,101,0,115,116,
114,101,110,103,116,104,0,112,104,97,115,101,0,111,102,102,115,101,116,0,114,111,117,103,104,110,101,115,115,0,108,97,99,117,110,97,114,105,116,121,0,100,101,112,116,104,0,109,111,100,105,102,105,99,97,116,105,111,110,0,108,101,103,97,99,121,95,110,111,105,115,101,0,115,116,101,112,95,115,105,122,101,0,42,105,100,0,112,99,104,97,110,95,110,97,109,101,91,54,52,93,0,116,114,97,110,115,67,104,97,110,0,114,111,116,97,116,105,111,110,
95,109,111,100,101,0,95,112,97,100,91,53,93,0,111,112,116,105,111,110,115,0,99,111,110,116,101,120,116,95,112,114,111,112,101,114,116,121,0,102,97,108,108,98,97,99,107,95,118,97,108,117,101,0,116,97,114,103,101,116,115,91,56,93,0,110,117,109,95,116,97,114,103,101,116,115,0,99,117,114,118,97,108,0,118,97,114,105,97,98,108,101,115,0,101,120,112,114,101,115,115,105,111,110,91,50,53,54,93,0,42,101,120,112,114,95,99,111,109,112,
0,42,101,120,112,114,95,115,105,109,112,108,101,0,118,101,99,91,50,93,0,42,100,114,105,118,101,114,0,109,111,100,105,102,105,101,114,115,0,42,98,101,122,116,0,42,102,112,116,0,97,99,116,105,118,101,95,107,101,121,102,114,97,109,101,95,105,110,100,101,120,0,101,120,116,101,110,100,0,97,117,116,111,95,115,109,111,111,116,104,105,110,103,0,97,114,114,97,121,95,105,110,100,101,120,0,99,111,108,111,114,95,109,111,100,101,0,112,114,101,
118,95,110,111,114,109,95,102,97,99,116,111,114,0,112,114,101,118,95,111,102,102,115,101,116,0,115,116,114,105,112,115,0,42,97,99,116,0,97,99,116,105,111,110,95,115,108,111,116,95,110,97,109,101,91,54,54,93,0,102,99,117,114,118,101,115,0,115,116,114,105,112,95,116,105,109,101,0,115,116,97,114,116,0,101,110,100,0,97,99,116,115,116,97,114,116,0,97,99,116,101,110,100,0,114,101,112,101,97,116,0,115,99,97,108,101,0,98,108,101,
110,100,109,111,100,101,0,101,120,116,101,110,100,109,111,100,101,0,95,112,97,100,49,91,50,93,0,42,115,112,101,97,107,101,114,95,104,97,110,100,108,101,0,42,111,114,105,103,95,115,116,114,105,112,0,42,95,112,97,100,51,0,105,110,100,101,120,0,103,114,111,117,112,91,54,52,93,0,103,114,111,117,112,109,111,100,101,0,107,101,121,105,110,103,102,108,97,103,0,107,101,121,105,110,103,111,118,101,114,114,105,100,101,0,112,97,116,104,115,0,
105,100,110,97,109,101,91,54,52,93,0,100,101,115,99,114,105,112,116,105,111,110,91,49,48,50,52,93,0,116,121,112,101,105,110,102,111,91,54,52,93,0,97,99,116,105,118,101,95,112,97,116,104,0,115,108,111,116,95,110,97,109,101,91,54,54,93,0,42,116,109,112,97,99,116,0,116,109,112,95,115,108,111,116,95,104,97,110,100,108,101,0,116,109,112,95,115,108,111,116,95,110,97,109,101,91,54,54,93,0,110,108,97,95,116,114,97,99,107,115,
0,42,97,99,116,95,116,114,97,99,107,0,42,97,99,116,115,116,114,105,112,0,100,114,105,118,101,114,115,0,111,118,101,114,114,105,100,101,115,0,42,42,100,114,105,118,101,114,95,97,114,114,97,121,0,97,99,116,95,98,108,101,110,100,109,111,100,101,0,97,99,116,95,101,120,116,101,110,100,109,111,100,101,0,97,99,116,95,105,110,102,108,117,101,110,99,101,0,42,97,100,116,0,112,97,108,101,116,116,101,95,105,110,100,101,120,0,99,117,115,
116,111,109,0,99,111,108,108,101,99,116,105,111,110,115,0,42,95,112,97,100,48,0,99,104,105,108,100,98,97,115,101,0,114,111,108,108,0,104,101,97,100,91,51,93,0,116,97,105,108,91,51,93,0,98,111,110,101,95,109,97,116,91,51,93,91,51,93,0,100,114,97,119,116,121,112,101,0,95,112,97,100,49,91,51,93,0,105,110,104,101,114,105,116,95,115,99,97,108,101,95,109,111,100,101,0,97,114,109,95,104,101,97,100,91,51,93,0,97,114,
109,95,116,97,105,108,91,51,93,0,97,114,109,95,109,97,116,91,52,93,91,52,93,0,97,114,109,95,114,111,108,108,0,100,105,115,116,0,119,101,105,103,104,116,0,120,119,105,100,116,104,0,122,119,105,100,116,104,0,114,97,100,95,104,101,97,100,0,114,97,100,95,116,97,105,108,0,108,97,121,101,114,0,115,101,103,109,101,110,116,115,0,98,98,111,110,101,95,109,97,112,112,105,110,103,95,109,111,100,101,0,95,112,97,100,50,91,55,93,0,
98,98,111,110,101,95,112,114,101,118,95,116,121,112,101,0,98,98,111,110,101,95,110,101,120,116,95,116,121,112,101,0,98,98,111,110,101,95,102,108,97,103,0,98,98,111,110,101,95,112,114,101,118,95,102,108,97,103,0,98,98,111,110,101,95,110,101,120,116,95,102,108,97,103,0,97,99,116,105,118,101,95,99,111,108,108,101,99,116,105,111,110,95,105,110,100,101,120,0,42,97,99,116,105,118,101,95,99,111,108,108,101,99,116,105,111,110,0,98,111,
110,101,98,97,115,101,0,42,98,111,110,101,104,97,115,104,0,42,101,100,98,111,0,42,97,99,116,95,98,111,110,101,0,42,97,99,116,95,101,100,98,111,110,101,0,110,101,101,100,115,95,102,108,117,115,104,95,116,111,95,105,100,0,100,101,102,111,114,109,102,108,97,103,0,112,97,116,104,102,108,97,103,0,42,42,99,111,108,108,101,99,116,105,111,110,95,97,114,114,97,121,0,99,111,108,108,101,99,116,105,111,110,95,97,114,114,97,121,95,110,
117,109,0,99,111,108,108,101,99,116,105,111,110,95,114,111,111,116,95,99,111,117,110,116,0,97,99,116,105,118,101,95,99,111,108,108,101,99,116,105,111,110,95,110,97,109,101,91,54,52,93,0,108,97,121,101,114,95,117,115,101,100,0,108,97,121,101,114,95,112,114,111,116,101,99,116,101,100,0,97,120,101,115,95,112,111,115,105,116,105,111,110,0,98,111,110,101,115,0,102,108,97,103,115,0,99,104,105,108,100,95,105,110,100,101,120,0,99,104,105,
108,100,95,99,111,117,110,116,0,42,98,99,111,108,108,0,42,108,111,99,97,108,95,116,121,112,101,95,105,110,102,111,0,99,97,116,97,108,111,103,95,105,100,0,99,97,116,97,108,111,103,95,115,105,109,112,108,101,95,110,97,109,101,91,54,52,93,0,42,97,117,116,104,111,114,0,42,99,111,112,121,114,105,103,104,116,0,42,108,105,99,101,110,115,101,0,116,97,103,115,0,97,99,116,105,118,101,95,116,97,103,0,116,111,116,95,116,97,103,115,
0,99,117,115,116,111,109,95,108,105,98,114,97,114,121,95,105,110,100,101,120,0,97,115,115,101,116,95,108,105,98,114,97,114,121,95,116,121,112,101,0,42,97,115,115,101,116,95,108,105,98,114,97,114,121,95,105,100,101,110,116,105,102,105,101,114,0,42,114,101,108,97,116,105,118,101,95,97,115,115,101,116,95,105,100,101,110,116,105,102,105,101,114,0,42,112,97,116,104,0,42,115,104,97,114,105,110,103,95,105,110,102,111,0,100,97,116,97,95,116,
121,112,101,0,100,111,109,97,105,110,0,115,116,111,114,97,103,101,95,116,121,112,101,0,42,100,110,97,95,97,116,116,114,105,98,117,116,101,115,0,100,110,97,95,97,116,116,114,105,98,117,116,101,115,95,110,117,109,0,110,97,109,101,91,51,50,93,0,114,117,108,101,0,42,111,98,0,102,101,97,114,95,102,97,99,116,111,114,0,115,105,103,110,97,108,95,105,100,0,108,111,111,107,95,97,104,101,97,100,0,111,108,111,99,91,51,93,0,99,102,
114,97,0,100,105,115,116,97,110,99,101,0,113,117,101,117,101,95,115,105,122,101,0,119,97,110,100,101,114,0,108,101,118,101,108,0,115,112,101,101,100,0,102,108,101,101,95,100,105,115,116,97,110,99,101,0,104,101,97,108,116,104,0,97,99,99,91,51,93,0,115,116,97,116,101,95,105,100,0,114,117,108,101,115,0,99,111,110,100,105,116,105,111,110,115,0,97,99,116,105,111,110,115,0,114,117,108,101,115,101,116,95,116,121,112,101,0,114,117,108,
101,95,102,117,122,122,105,110,101,115,115,0,118,111,108,117,109,101,0,102,97,108,108,111,102,102,0,108,97,115,116,95,115,116,97,116,101,95,105,100,0,108,97,110,100,105,110,103,95,115,109,111,111,116,104,110,101,115,115,0,104,101,105,103,104,116,0,98,97,110,107,105,110,103,0,112,105,116,99,104,0,97,103,103,114,101,115,115,105,111,110,0,97,99,99,117,114,97,99,121,0,114,97,110,103,101,0,97,105,114,95,109,105,110,95,115,112,101,101,100,
0,97,105,114,95,109,97,120,95,115,112,101,101,100,0,97,105,114,95,109,97,120,95,97,99,99,0,97,105,114,95,109,97,120,95,97,118,101,0,97,105,114,95,112,101,114,115,111,110,97,108,95,115,112,97,99,101,0,108,97,110,100,95,106,117,109,112,95,115,112,101,101,100,0,108,97,110,100,95,109,97,120,95,115,112,101,101,100,0,108,97,110,100,95,109,97,120,95,97,99,99,0,108,97,110,100,95,109,97,120,95,97,118,101,0,108,97,110,100,95,
112,101,114,115,111,110,97,108,95,115,112,97,99,101,0,108,97,110,100,95,115,116,105,99,107,95,102,111,114,99,101,0,115,116,97,116,101,115,0,100,114,97,119,95,115,109,111,111,116,104,102,97,99,0,102,105,108,108,95,102,97,99,116,111,114,0,100,114,97,119,95,115,116,114,101,110,103,116,104,0,100,114,97,119,95,106,105,116,116,101,114,0,100,114,97,119,95,97,110,103,108,101,0,100,114,97,119,95,97,110,103,108,101,95,102,97,99,116,111,114,
0,100,114,97,119,95,114,97,110,100,111,109,95,112,114,101,115,115,0,100,114,97,119,95,114,97,110,100,111,109,95,115,116,114,101,110,103,116,104,0,100,114,97,119,95,115,109,111,111,116,104,108,118,108,0,100,114,97,119,95,115,117,98,100,105,118,105,100,101,0,102,105,108,108,95,108,97,121,101,114,95,109,111,100,101,0,102,105,108,108,95,100,105,114,101,99,116,105,111,110,0,102,105,108,108,95,116,104,114,101,115,104,111,108,100,0,95,112,97,100,
50,91,50,93,0,99,97,112,115,95,116,121,112,101,0,95,112,97,100,91,49,93,0,102,108,97,103,50,0,102,105,108,108,95,115,105,109,112,108,121,108,118,108,0,102,105,108,108,95,100,114,97,119,95,109,111,100,101,0,102,105,108,108,95,101,120,116,101,110,100,95,109,111,100,101,0,105,110,112,117,116,95,115,97,109,112,108,101,115,0,117,118,95,114,97,110,100,111,109,0,98,114,117,115,104,95,116,121,112,101,0,101,114,97,115,101,114,95,109,111,
100,101,0,97,99,116,105,118,101,95,115,109,111,111,116,104,0,101,114,97,95,115,116,114,101,110,103,116,104,95,102,0,101,114,97,95,116,104,105,99,107,110,101,115,115,95,102,0,103,114,97,100,105,101,110,116,95,102,0,103,114,97,100,105,101,110,116,95,115,91,50,93,0,115,105,109,112,108,105,102,121,95,102,0,118,101,114,116,101,120,95,102,97,99,116,111,114,0,118,101,114,116,101,120,95,109,111,100,101,0,115,99,117,108,112,116,95,102,108,97,
103,0,115,99,117,108,112,116,95,109,111,100,101,95,102,108,97,103,0,112,114,101,115,101,116,95,116,121,112,101,0,98,114,117,115,104,95,100,114,97,119,95,109,111,100,101,0,114,97,110,100,111,109,95,104,117,101,0,114,97,110,100,111,109,95,115,97,116,117,114,97,116,105,111,110,0,114,97,110,100,111,109,95,118,97,108,117,101,0,99,111,108,111,114,95,106,105,116,116,101,114,95,102,108,97,103,0,102,105,108,108,95,101,120,116,101,110,100,95,102,
97,99,0,100,105,108,97,116,101,95,112,105,120,101,108,115,0,42,99,117,114,118,101,95,115,101,110,115,105,116,105,118,105,116,121,0,42,99,117,114,118,101,95,115,116,114,101,110,103,116,104,0,42,99,117,114,118,101,95,106,105,116,116,101,114,0,42,99,117,114,118,101,95,114,97,110,100,95,112,114,101,115,115,117,114,101,0,42,99,117,114,118,101,95,114,97,110,100,95,115,116,114,101,110,103,116,104,0,42,99,117,114,118,101,95,114,97,110,100,95,
117,118,0,42,99,117,114,118,101,95,114,97,110,100,95,104,117,101,0,42,99,117,114,118,101,95,114,97,110,100,95,115,97,116,117,114,97,116,105,111,110,0,42,99,117,114,118,101,95,114,97,110,100,95,118,97,108,117,101,0,111,117,116,108,105,110,101,95,102,97,99,0,115,105,109,112,108,105,102,121,95,112,120,0,42,109,97,116,101,114,105,97,108,0,42,109,97,116,101,114,105,97,108,95,97,108,116,0,97,100,100,95,97,109,111,117,110,116,0,112,
111,105,110,116,115,95,112,101,114,95,99,117,114,118,101,0,109,105,110,105,109,117,109,95,108,101,110,103,116,104,0,99,117,114,118,101,95,108,101,110,103,116,104,0,109,105,110,105,109,117,109,95,100,105,115,116,97,110,99,101,0,99,117,114,118,101,95,114,97,100,105,117,115,0,100,101,110,115,105,116,121,95,97,100,100,95,97,116,116,101,109,112,116,115,0,100,101,110,115,105,116,121,95,109,111,100,101,0,95,112,97,100,91,55,93,0,42,99,117,114,
118,101,95,112,97,114,97,109,101,116,101,114,95,102,97,108,108,111,102,102,0,109,116,101,120,0,109,97,115,107,95,109,116,101,120,0,42,116,111,103,103,108,101,95,98,114,117,115,104,0,42,105,99,111,110,95,105,109,98,117,102,0,42,103,114,97,100,105,101,110,116,0,42,112,97,105,110,116,95,99,117,114,118,101,0,105,99,111,110,95,102,105,108,101,112,97,116,104,91,49,48,50,52,93,0,110,111,114,109,97,108,95,119,101,105,103,104,116,0,114,
97,107,101,95,102,97,99,116,111,114,0,98,108,101,110,100,0,111,98,95,109,111,100,101,0,115,97,109,112,108,105,110,103,95,102,108,97,103,0,109,97,115,107,95,112,114,101,115,115,117,114,101,0,106,105,116,116,101,114,0,106,105,116,116,101,114,95,97,98,115,111,108,117,116,101,0,111,118,101,114,108,97,121,95,102,108,97,103,115,0,115,112,97,99,105,110,103,0,115,109,111,111,116,104,95,115,116,114,111,107,101,95,114,97,100,105,117,115,0,115,
109,111,111,116,104,95,115,116,114,111,107,101,95,102,97,99,116,111,114,0,114,97,116,101,0,114,103,98,91,51,93,0,104,115,118,95,106,105,116,116,101,114,91,51,93,0,97,108,112,104,97,0,104,97,114,100,110,101,115,115,0,102,108,111,119,0,119,101,116,95,109,105,120,0,119,101,116,95,112,101,114,115,105,115,116,101,110,99,101,0,100,101,110,115,105,116,121,0,112,97,105,110,116,95,102,108,97,103,115,0,116,105,112,95,114,111,117,110,100,110,
101,115,115,0,116,105,112,95,115,99,97,108,101,95,120,0,115,101,99,111,110,100,97,114,121,95,114,103,98,91,51,93,0,100,97,115,104,95,114,97,116,105,111,0,100,97,115,104,95,115,97,109,112,108,101,115,0,115,99,117,108,112,116,95,112,108,97,110,101,0,103,114,97,100,105,101,110,116,95,115,112,97,99,105,110,103,0,103,114,97,100,105,101,110,116,95,115,116,114,111,107,101,95,109,111,100,101,0,103,114,97,100,105,101,110,116,95,102,105,108,
108,95,109,111,100,101,0,104,97,115,95,117,110,115,97,118,101,100,95,99,104,97,110,103,101,115,0,102,97,108,108,111,102,102,95,115,104,97,112,101,0,102,97,108,108,111,102,102,95,97,110,103,108,101,0,115,99,117,108,112,116,95,116,111,111,108,0,118,101,114,116,101,120,112,97,105,110,116,95,116,111,111,108,0,119,101,105,103,104,116,112,97,105,110,116,95,116,111,111,108,0,105,109,97,103,101,112,97,105,110,116,95,116,111,111,108,0,109,97,115,
107,95,116,111,111,108,0,103,112,101,110,99,105,108,95,116,111,111,108,0,103,112,101,110,99,105,108,95,118,101,114,116,101,120,95,116,111,111,108,0,103,112,101,110,99,105,108,95,115,99,117,108,112,116,95,116,111,111,108,0,103,112,101,110,99,105,108,95,119,101,105,103,104,116,95,116,111,111,108,0,99,117,114,118,101,115,95,115,99,117,108,112,116,95,116,111,111,108,0,95,112,97,100,49,91,49,48,93,0,97,117,116,111,115,109,111,111,116,104,95,
102,97,99,116,111,114,0,116,105,108,116,95,115,116,114,101,110,103,116,104,95,102,97,99,116,111,114,0,116,111,112,111,108,111,103,121,95,114,97,107,101,95,102,97,99,116,111,114,0,99,114,101,97,115,101,95,112,105,110,99,104,95,102,97,99,116,111,114,0,110,111,114,109,97,108,95,114,97,100,105,117,115,95,102,97,99,116,111,114,0,97,114,101,97,95,114,97,100,105,117,115,95,102,97,99,116,111,114,0,119,101,116,95,112,97,105,110,116,95,114,
97,100,105,117,115,95,102,97,99,116,111,114,0,112,108,97,110,101,95,116,114,105,109,0,112,108,97,110,101,95,104,101,105,103,104,116,0,112,108,97,110,101,95,100,101,112,116,104,0,115,116,97,98,105,108,105,122,101,95,110,111,114,109,97,108,0,115,116,97,98,105,108,105,122,101,95,112,108,97,110,101,0,112,108,97,110,101,95,105,110,118,101,114,115,105,111,110,95,109,111,100,101,0,116,101,120,116,117,114,101,95,115,97,109,112,108,101,95,98,105,
97,115,0,99,117,114,118,101,95,112,114,101,115,101,116,0,100,105,115,99,111,110,110,101,99,116,101,100,95,100,105,115,116,97,110,99,101,95,109,97,120,0,100,101,102,111,114,109,95,116,97,114,103,101,116,0,97,117,116,111,109,97,115,107,105,110,103,95,102,108,97,103,115,0,97,117,116,111,109,97,115,107,105,110,103,95,98,111,117,110,100,97,114,121,95,101,100,103,101,115,95,112,114,111,112,97,103,97,116,105,111,110,95,115,116,101,112,115,0,97,
117,116,111,109,97,115,107,105,110,103,95,115,116,97,114,116,95,110,111,114,109,97,108,95,108,105,109,105,116,0,97,117,116,111,109,97,115,107,105,110,103,95,115,116,97,114,116,95,110,111,114,109,97,108,95,102,97,108,108,111,102,102,0,97,117,116,111,109,97,115,107,105,110,103,95,118,105,101,119,95,110,111,114,109,97,108,95,108,105,109,105,116,0,97,117,116,111,109,97,115,107,105,110,103,95,118,105,101,119,95,110,111,114,109,97,108,95,102,97,108,
108,111,102,102,0,101,108,97,115,116,105,99,95,100,101,102,111,114,109,95,116,121,112,101,0,101,108,97,115,116,105,99,95,100,101,102,111,114,109,95,118,111,108,117,109,101,95,112,114,101,115,101,114,118,97,116,105,111,110,0,115,110,97,107,101,95,104,111,111,107,95,100,101,102,111,114,109,95,116,121,112,101,0,112,111,115,101,95,100,101,102,111,114,109,95,116,121,112,101,0,112,111,115,101,95,111,102,102,115,101,116,0,112,111,115,101,95,115,109,111,
111,116,104,95,105,116,101,114,97,116,105,111,110,115,0,112,111,115,101,95,105,107,95,115,101,103,109,101,110,116,115,0,112,111,115,101,95,111,114,105,103,105,110,95,116,121,112,101,0,98,111,117,110,100,97,114,121,95,100,101,102,111,114,109,95,116,121,112,101,0,98,111,117,110,100,97,114,121,95,102,97,108,108,111,102,102,95,116,121,112,101,0,98,111,117,110,100,97,114,121,95,111,102,102,115,101,116,0,99,108,111,116,104,95,100,101,102,111,114,109,
95,116,121,112,101,0,99,108,111,116,104,95,102,111,114,99,101,95,102,97,108,108,111,102,102,95,116,121,112,101,0,99,108,111,116,104,95,115,105,109,117,108,97,116,105,111,110,95,97,114,101,97,95,116,121,112,101,0,99,108,111,116,104,95,109,97,115,115,0,99,108,111,116,104,95,100,97,109,112,105,110,103,0,99,108,111,116,104,95,115,105,109,95,108,105,109,105,116,0,99,108,111,116,104,95,115,105,109,95,102,97,108,108,111,102,102,0,99,108,111,
116,104,95,99,111,110,115,116,114,97,105,110,116,95,115,111,102,116,98,111,100,121,95,115,116,114,101,110,103,116,104,0,115,109,111,111,116,104,95,100,101,102,111,114,109,95,116,121,112,101,0,115,117,114,102,97,99,101,95,115,109,111,111,116,104,95,115,104,97,112,101,95,112,114,101,115,101,114,118,97,116,105,111,110,0,115,117,114,102,97,99,101,95,115,109,111,111,116,104,95,99,117,114,114,101,110,116,95,118,101,114,116,101,120,0,115,117,114,102,97,
99,101,95,115,109,111,111,116,104,95,105,116,101,114,97,116,105,111,110,115,0,109,117,108,116,105,112,108,97,110,101,95,115,99,114,97,112,101,95,97,110,103,108,101,0,115,109,101,97,114,95,100,101,102,111,114,109,95,116,121,112,101,0,115,108,105,100,101,95,100,101,102,111,114,109,95,116,121,112,101,0,116,101,120,116,117,114,101,95,111,118,101,114,108,97,121,95,97,108,112,104,97,0,109,97,115,107,95,111,118,101,114,108,97,121,95,97,108,112,104,
97,0,99,117,114,115,111,114,95,111,118,101,114,108,97,121,95,97,108,112,104,97,0,117,110,112,114,111,106,101,99,116,101,100,95,114,97,100,105,117,115,0,115,104,97,114,112,95,116,104,114,101,115,104,111,108,100,0,98,108,117,114,95,107,101,114,110,101,108,95,114,97,100,105,117,115,0,98,108,117,114,95,109,111,100,101,0,97,100,100,95,99,111,108,91,52,93,0,115,117,98,95,99,111,108,91,52,93,0,115,116,101,110,99,105,108,95,112,111,115,
91,50,93,0,115,116,101,110,99,105,108,95,100,105,109,101,110,115,105,111,110,91,50,93,0,109,97,115,107,95,115,116,101,110,99,105,108,95,112,111,115,91,50,93,0,109,97,115,107,95,115,116,101,110,99,105,108,95,100,105,109,101,110,115,105,111,110,91,50,93,0,42,103,112,101,110,99,105,108,95,115,101,116,116,105,110,103,115,0,42,99,117,114,118,101,115,95,115,99,117,108,112,116,95,115,101,116,116,105,110,103,115,0,97,117,116,111,109,97,115,
107,105,110,103,95,99,97,118,105,116,121,95,98,108,117,114,95,115,116,101,112,115,0,97,117,116,111,109,97,115,107,105,110,103,95,99,97,118,105,116,121,95,102,97,99,116,111,114,0,42,97,117,116,111,109,97,115,107,105,110,103,95,99,97,118,105,116,121,95,99,117,114,118,101,0,104,0,115,0,118,0,99,111,108,111,114,115,0,97,99,116,105,118,101,95,99,111,108,111,114,0,98,101,122,0,112,114,101,115,115,117,114,101,0,116,111,116,95,112,111,
105,110,116,115,0,97,100,100,95,105,110,100,101,120,0,112,97,116,104,91,52,48,57,54,93,0,102,105,108,101,112,97,116,104,91,49,48,50,52,93,0,95,112,97,100,0,111,98,106,101,99,116,95,112,97,116,104,115,0,108,97,121,101,114,115,0,105,115,95,115,101,113,117,101,110,99,101,0,102,111,114,119,97,114,100,95,97,120,105,115,0,117,112,95,97,120,105,115,0,111,118,101,114,114,105,100,101,95,102,114,97,109,101,0,102,114,97,109,101,0,
117,115,101,95,114,101,110,100,101,114,95,112,114,111,99,101,100,117,114,97,108,0,117,115,101,95,112,114,101,102,101,116,99,104,0,112,114,101,102,101,116,99,104,95,99,97,99,104,101,95,115,105,122,101,0,97,99,116,105,118,101,95,108,97,121,101,114,0,95,112,97,100,50,91,51,93,0,118,101,108,111,99,105,116,121,95,117,110,105,116,0,118,101,108,111,99,105,116,121,95,110,97,109,101,91,54,52,93,0,42,104,97,110,100,108,101,0,104,97,110,
100,108,101,95,102,105,108,101,112,97,116,104,91,49,48,50,52,93,0,42,104,97,110,100,108,101,95,114,101,97,100,101,114,115,0,105,110,116,101,114,111,99,117,108,97,114,95,100,105,115,116,97,110,99,101,0,99,111,110,118,101,114,103,101,110,99,101,95,100,105,115,116,97,110,99,101,0,99,111,110,118,101,114,103,101,110,99,101,95,109,111,100,101,0,112,105,118,111,116,0,112,111,108,101,95,109,101,114,103,101,95,97,110,103,108,101,95,102,114,111,
109,0,112,111,108,101,95,109,101,114,103,101,95,97,110,103,108,101,95,116,111,0,42,105,109,97,0,105,117,115,101,114,0,42,99,108,105,112,0,99,117,115,101,114,0,111,102,102,115,101,116,91,50,93,0,114,111,116,97,116,105,111,110,0,115,111,117,114,99,101,0,42,102,111,99,117,115,95,111,98,106,101,99,116,0,102,111,99,117,115,95,115,117,98,116,97,114,103,101,116,91,54,52,93,0,102,111,99,117,115,95,100,105,115,116,97,110,99,101,0,
97,112,101,114,116,117,114,101,95,102,115,116,111,112,0,97,112,101,114,116,117,114,101,95,114,111,116,97,116,105,111,110,0,97,112,101,114,116,117,114,101,95,114,97,116,105,111,0,97,112,101,114,116,117,114,101,95,98,108,97,100,101,115,0,100,114,119,95,99,111,114,110,101,114,115,91,50,93,91,52,93,91,50,93,0,100,114,119,95,116,114,105,97,91,50,93,91,50,93,0,100,114,119,95,100,101,112,116,104,91,50,93,0,100,114,119,95,102,111,99,
117,115,109,97,116,91,52,93,91,52,93,0,100,114,119,95,110,111,114,109,97,108,109,97,116,91,52,93,91,52,93,0,100,116,120,0,112,97,115,115,101,112,97,114,116,97,108,112,104,97,0,99,108,105,112,115,116,97,0,99,108,105,112,101,110,100,0,108,101,110,115,0,111,114,116,104,111,95,115,99,97,108,101,0,100,114,97,119,115,105,122,101,0,115,101,110,115,111,114,95,120,0,115,101,110,115,111,114,95,121,0,115,104,105,102,116,120,0,115,104,
105,102,116,121,0,89,70,95,100,111,102,100,105,115,116,0,115,101,110,115,111,114,95,102,105,116,0,112,97,110,111,114,97,109,97,95,116,121,112,101,0,102,105,115,104,101,121,101,95,102,111,118,0,102,105,115,104,101,121,101,95,108,101,110,115,0,108,97,116,105,116,117,100,101,95,109,105,110,0,108,97,116,105,116,117,100,101,95,109,97,120,0,108,111,110,103,105,116,117,100,101,95,109,105,110,0,108,111,110,103,105,116,117,100,101,95,109,97,120,0,
102,105,115,104,101,121,101,95,112,111,108,121,110,111,109,105,97,108,95,107,48,0,102,105,115,104,101,121,101,95,112,111,108,121,110,111,109,105,97,108,95,107,49,0,102,105,115,104,101,121,101,95,112,111,108,121,110,111,109,105,97,108,95,107,50,0,102,105,115,104,101,121,101,95,112,111,108,121,110,111,109,105,97,108,95,107,51,0,102,105,115,104,101,121,101,95,112,111,108,121,110,111,109,105,97,108,95,107,52,0,99,101,110,116,114,97,108,95,99,121,
108,105,110,100,114,105,99,97,108,95,114,97,110,103,101,95,117,95,109,105,110,0,99,101,110,116,114,97,108,95,99,121,108,105,110,100,114,105,99,97,108,95,114,97,110,103,101,95,117,95,109,97,120,0,99,101,110,116,114,97,108,95,99,121,108,105,110,100,114,105,99,97,108,95,114,97,110,103,101,95,118,95,109,105,110,0,99,101,110,116,114,97,108,95,99,121,108,105,110,100,114,105,99,97,108,95,114,97,110,103,101,95,118,95,109,97,120,0,99,101,
110,116,114,97,108,95,99,121,108,105,110,100,114,105,99,97,108,95,114,97,100,105,117,115,0,95,112,97,100,50,0,42,99,117,115,116,111,109,95,115,104,97,100,101,114,0,99,117,115,116,111,109,95,102,105,108,101,112,97,116,104,91,49,48,50,52,93,0,99,117,115,116,111,109,95,98,121,116,101,99,111,100,101,95,104,97,115,104,91,54,52,93,0,42,99,117,115,116,111,109,95,98,121,116,101,99,111,100,101,0,99,117,115,116,111,109,95,109,111,100,
101,0,95,112,97,100,51,0,42,100,111,102,95,111,98,0,103,112,117,95,100,111,102,0,100,111,102,0,98,103,95,105,109,97,103,101,115,0,115,116,101,114,101,111,0,42,99,97,99,104,101,0,109,105,110,103,111,97,108,0,67,100,105,115,0,67,118,105,0,103,114,97,118,105,116,121,91,51,93,0,100,116,0,109,97,115,115,0,115,116,114,117,99,116,117,114,97,108,0,115,104,101,97,114,0,98,101,110,100,105,110,103,0,109,97,120,95,98,101,110,
100,0,109,97,120,95,115,116,114,117,99,116,0,109,97,120,95,115,104,101,97,114,0,109,97,120,95,115,101,119,105,110,103,0,97,118,103,95,115,112,114,105,110,103,95,108,101,110,0,116,105,109,101,115,99,97,108,101,0,116,105,109,101,95,115,99,97,108,101,0,109,97,120,103,111,97,108,0,101,102,102,95,102,111,114,99,101,95,115,99,97,108,101,0,101,102,102,95,119,105,110,100,95,115,99,97,108,101,0,115,105,109,95,116,105,109,101,95,111,108,
100,0,100,101,102,103,111,97,108,0,103,111,97,108,115,112,114,105,110,103,0,103,111,97,108,102,114,105,99,116,0,118,101,108,111,99,105,116,121,95,115,109,111,111,116,104,0,100,101,110,115,105,116,121,95,116,97,114,103,101,116,0,100,101,110,115,105,116,121,95,115,116,114,101,110,103,116,104,0,99,111,108,108,105,100,101,114,95,102,114,105,99,116,105,111,110,0,118,101,108,95,100,97,109,112,105,110,103,0,115,104,114,105,110,107,95,109,105,110,0,
115,104,114,105,110,107,95,109,97,120,0,117,110,105,102,111,114,109,95,112,114,101,115,115,117,114,101,95,102,111,114,99,101,0,116,97,114,103,101,116,95,118,111,108,117,109,101,0,112,114,101,115,115,117,114,101,95,102,97,99,116,111,114,0,102,108,117,105,100,95,100,101,110,115,105,116,121,0,118,103,114,111,117,112,95,112,114,101,115,115,117,114,101,0,95,112,97,100,55,91,54,93,0,98,101,110,100,105,110,103,95,100,97,109,112,105,110,103,0,118,
111,120,101,108,95,99,101,108,108,95,115,105,122,101,0,115,116,101,112,115,80,101,114,70,114,97,109,101,0,112,114,101,114,111,108,108,0,109,97,120,115,112,114,105,110,103,108,101,110,0,115,111,108,118,101,114,95,116,121,112,101,0,118,103,114,111,117,112,95,98,101,110,100,0,118,103,114,111,117,112,95,109,97,115,115,0,118,103,114,111,117,112,95,115,116,114,117,99,116,0,118,103,114,111,117,112,95,115,104,114,105,110,107,0,115,104,97,112,101,107,
101,121,95,114,101,115,116,0,112,114,101,115,101,116,115,0,114,101,115,101,116,0,42,101,102,102,101,99,116,111,114,95,119,101,105,103,104,116,115,0,98,101,110,100,105,110,103,95,109,111,100,101,108,0,118,103,114,111,117,112,95,115,104,101,97,114,0,116,101,110,115,105,111,110,0,99,111,109,112,114,101,115,115,105,111,110,0,109,97,120,95,116,101,110,115,105,111,110,0,109,97,120,95,99,111,109,112,114,101,115,115,105,111,110,0,116,101,110,115,105,
111,110,95,100,97,109,112,0,99,111,109,112,114,101,115,115,105,111,110,95,100,97,109,112,0,115,104,101,97,114,95,100,97,109,112,0,105,110,116,101,114,110,97,108,95,115,112,114,105,110,103,95,109,97,120,95,108,101,110,103,116,104,0,105,110,116,101,114,110,97,108,95,115,112,114,105,110,103,95,109,97,120,95,100,105,118,101,114,115,105,111,110,0,118,103,114,111,117,112,95,105,110,116,101,114,110,0,105,110,116,101,114,110,97,108,95,116,101,110,115,
105,111,110,0,105,110,116,101,114,110,97,108,95,99,111,109,112,114,101,115,115,105,111,110,0,109,97,120,95,105,110,116,101,114,110,97,108,95,116,101,110,115,105,111,110,0,109,97,120,95,105,110,116,101,114,110,97,108,95,99,111,109,112,114,101,115,115,105,111,110,0,42,99,111,108,108,105,115,105,111,110,95,108,105,115,116,0,101,112,115,105,108,111,110,0,115,101,108,102,95,102,114,105,99,116,105,111,110,0,102,114,105,99,116,105,111,110,0,100,97,
109,112,105,110,103,0,115,101,108,102,101,112,115,105,108,111,110,0,114,101,112,101,108,95,102,111,114,99,101,0,100,105,115,116,97,110,99,101,95,114,101,112,101,108,0,115,101,108,102,95,108,111,111,112,95,99,111,117,110,116,0,108,111,111,112,95,99,111,117,110,116,0,42,103,114,111,117,112,0,118,103,114,111,117,112,95,115,101,108,102,99,111,108,0,118,103,114,111,117,112,95,111,98,106,99,111,108,0,99,108,97,109,112,0,115,101,108,102,95,99,
108,97,109,112,0,108,105,110,107,95,115,116,97,116,101,0,108,105,103,104,116,95,108,105,110,107,105,110,103,0,42,99,111,108,108,101,99,116,105,111,110,0,102,104,95,105,100,110,97,109,101,91,54,52,93,0,42,101,120,112,111,114,116,95,112,114,111,112,101,114,116,105,101,115,0,95,112,97,100,48,0,111,98,106,101,99,116,95,99,97,99,104,101,0,111,98,106,101,99,116,95,99,97,99,104,101,95,105,110,115,116,97,110,99,101,100,0,112,97,114,
101,110,116,115,0,42,103,111,98,106,101,99,116,95,104,97,115,104,0,42,111,119,110,101,114,95,105,100,0,103,111,98,106,101,99,116,0,99,104,105,108,100,114,101,110,0,97,99,116,105,118,101,95,101,120,112,111,114,116,101,114,95,105,110,100,101,120,0,101,120,112,111,114,116,101,114,115,0,100,117,112,108,105,95,111,102,115,91,51,93,0,99,111,108,111,114,95,116,97,103,0,108,105,110,101,97,114,116,95,117,115,97,103,101,0,108,105,110,101,97,
114,116,95,102,108,97,103,115,0,108,105,110,101,97,114,116,95,105,110,116,101,114,115,101,99,116,105,111,110,95,109,97,115,107,0,108,105,110,101,97,114,116,95,105,110,116,101,114,115,101,99,116,105,111,110,95,112,114,105,111,114,105,116,121,0,42,118,105,101,119,95,108,97,121,101,114,0,120,0,121,0,115,104,111,114,116,121,0,116,111,116,112,111,105,110,116,0,109,105,110,116,97,98,108,101,0,109,97,120,116,97,98,108,101,0,101,120,116,95,105,
110,91,50,93,0,101,120,116,95,111,117,116,91,50,93,0,42,116,97,98,108,101,0,42,112,114,101,109,117,108,116,97,98,108,101,0,112,114,101,109,117,108,95,101,120,116,95,105,110,91,50,93,0,112,114,101,109,117,108,95,101,120,116,95,111,117,116,91,50,93,0,100,101,102,97,117,108,116,95,104,97,110,100,108,101,95,116,121,112,101,0,99,117,114,0,112,114,101,115,101,116,0,99,104,97,110,103,101,100,95,116,105,109,101,115,116,97,109,112,0,
99,117,114,114,0,99,108,105,112,114,0,99,109,91,52,93,0,98,108,97,99,107,91,51,93,0,119,104,105,116,101,91,51,93,0,98,119,109,117,108,91,51,93,0,115,97,109,112,108,101,91,51,93,0,116,111,110,101,0,120,95,114,101,115,111,108,117,116,105,111,110,0,100,97,116,97,95,108,117,109,97,91,50,53,54,93,0,100,97,116,97,95,114,91,50,53,54,93,0,100,97,116,97,95,103,91,50,53,54,93,0,100,97,116,97,95,98,91,50,53,
54,93,0,100,97,116,97,95,97,91,50,53,54,93,0,120,109,97,120,0,121,109,97,120,0,99,111,91,50,93,91,50,93,0,111,107,0,115,97,109,112,108,101,95,102,117,108,108,0,115,97,109,112,108,101,95,108,105,110,101,115,0,119,97,118,101,102,114,109,95,109,111,100,101,0,118,101,99,115,99,111,112,101,95,109,111,100,101,0,119,97,118,101,102,114,109,95,104,101,105,103,104,116,0,118,101,99,115,99,111,112,101,95,104,101,105,103,104,116,0,
119,97,118,101,102,111,114,109,95,116,111,116,0,119,97,118,101,102,114,109,95,97,108,112,104,97,0,119,97,118,101,102,114,109,95,121,102,97,99,0,118,101,99,115,99,111,112,101,95,97,108,112,104,97,0,109,105,110,109,97,120,91,51,93,91,50,93,0,104,105,115,116,0,42,119,97,118,101,102,111,114,109,95,49,0,42,119,97,118,101,102,111,114,109,95,50,0,42,119,97,118,101,102,111,114,109,95,51,0,42,118,101,99,115,99,111,112,101,0,42,
118,101,99,115,99,111,112,101,95,114,103,98,0,108,111,111,107,91,54,52,93,0,118,105,101,119,95,116,114,97,110,115,102,111,114,109,91,54,52,93,0,101,120,112,111,115,117,114,101,0,103,97,109,109,97,0,116,101,109,112,101,114,97,116,117,114,101,0,116,105,110,116,0,42,99,117,114,118,101,95,109,97,112,112,105,110,103,0,100,105,115,112,108,97,121,95,100,101,118,105,99,101,91,54,52,93,0,114,0,103,0,98,0,97,0,112,111,115,0,116,
111,116,0,105,112,111,116,121,112,101,0,105,112,111,116,121,112,101,95,104,117,101,0,100,97,116,97,91,51,50,93,0,110,97,109,101,91,51,48,93,0,111,119,110,115,112,97,99,101,0,116,97,114,115,112,97,99,101,0,42,115,112,97,99,101,95,111,98,106,101,99,116,0,115,112,97,99,101,95,115,117,98,116,97,114,103,101,116,91,54,52,93,0,101,110,102,111,114,99,101,0,104,101,97,100,116,97,105,108,0,108,105,110,95,101,114,114,111,114,0,
114,111,116,95,101,114,114,111,114,0,42,116,97,114,0,115,117,98,116,97,114,103,101,116,91,54,52,93,0,109,97,116,114,105,120,91,52,93,91,52,93,0,115,112,97,99,101,0,114,111,116,79,114,100,101,114,0,105,116,101,114,97,116,105,111,110,115,0,114,111,111,116,98,111,110,101,0,109,97,120,95,114,111,111,116,98,111,110,101,0,42,112,111,108,101,116,97,114,0,112,111,108,101,115,117,98,116,97,114,103,101,116,91,54,52,93,0,112,111,108,
101,97,110,103,108,101,0,111,114,105,101,110,116,119,101,105,103,104,116,0,103,114,97,98,116,97,114,103,101,116,91,51,93,0,110,117,109,112,111,105,110,116,115,0,99,104,97,105,110,108,101,110,0,120,122,83,99,97,108,101,77,111,100,101,0,121,83,99,97,108,101,77,111,100,101,0,98,117,108,103,101,0,98,117,108,103,101,95,109,105,110,0,98,117,108,103,101,95,109,97,120,0,98,117,108,103,101,95,115,109,111,111,116,104,0,116,97,114,103,101,
116,115,0,114,101,115,101,114,118,101,100,49,0,114,101,115,101,114,118,101,100,50,0,101,117,108,101,114,95,111,114,100,101,114,0,109,105,120,95,109,111,100,101,0,112,111,119,101,114,0,109,105,110,109,97,120,102,108,97,103,0,108,111,99,97,108,0,101,118,97,108,95,116,105,109,101,0,116,114,97,99,107,102,108,97,103,0,108,111,99,107,102,108,97,103,0,111,102,102,115,101,116,95,102,97,99,0,102,111,108,108,111,119,102,108,97,103,0,117,112,
102,108,97,103,0,118,111,108,109,111,100,101,0,112,108,97,110,101,0,111,114,103,108,101,110,103,116,104,0,112,105,118,88,0,112,105,118,89,0,112,105,118,90,0,97,120,88,0,97,120,89,0,97,120,90,0,109,105,110,76,105,109,105,116,91,54,93,0,109,97,120,76,105,109,105,116,91,54,93,0,101,120,116,114,97,70,122,0,105,110,118,109,97,116,91,52,93,91,52,93,0,102,114,111,109,0,116,111,0,109,97,112,91,51,93,0,101,120,112,111,
0,102,114,111,109,95,114,111,116,97,116,105,111,110,95,109,111,100,101,0,116,111,95,101,117,108,101,114,95,111,114,100,101,114,0,109,105,120,95,109,111,100,101,95,108,111,99,0,109,105,120,95,109,111,100,101,95,114,111,116,0,109,105,120,95,109,111,100,101,95,115,99,97,108,101,0,102,114,111,109,95,109,105,110,91,51,93,0,102,114,111,109,95,109,97,120,91,51,93,0,116,111,95,109,105,110,91,51,93,0,116,111,95,109,97,120,91,51,93,0,
102,114,111,109,95,109,105,110,95,114,111,116,91,51,93,0,102,114,111,109,95,109,97,120,95,114,111,116,91,51,93,0,116,111,95,109,105,110,95,114,111,116,91,51,93,0,116,111,95,109,97,120,95,114,111,116,91,51,93,0,102,114,111,109,95,109,105,110,95,115,99,97,108,101,91,51,93,0,102,114,111,109,95,109,97,120,95,115,99,97,108,101,91,51,93,0,116,111,95,109,105,110,95,115,99,97,108,101,91,51,93,0,116,111,95,109,97,120,95,115,
99,97,108,101,91,51,93,0,111,102,102,115,101,116,91,51,93,0,114,111,116,65,120,105,115,0,120,109,105,110,0,121,109,105,110,0,122,109,105,110,0,122,109,97,120,0,115,111,102,116,0,42,116,97,114,103,101,116,0,115,104,114,105,110,107,84,121,112,101,0,112,114,111,106,65,120,105,115,0,112,114,111,106,65,120,105,115,83,112,97,99,101,0,112,114,111,106,76,105,109,105,116,0,115,104,114,105,110,107,77,111,100,101,0,116,114,97,99,107,65,
120,105,115,0,116,114,97,99,107,91,54,52,93,0,102,114,97,109,101,95,109,101,116,104,111,100,0,111,98,106,101,99,116,91,54,52,93,0,42,99,97,109,101,114,97,0,42,100,101,112,116,104,95,111,98,0,42,99,97,99,104,101,95,102,105,108,101,0,111,98,106,101,99,116,95,112,97,116,104,91,49,48,50,52,93,0,42,114,101,97,100,101,114,0,114,101,97,100,101,114,95,111,98,106,101,99,116,95,112,97,116,104,91,49,48,50,52,93,0,118,
101,99,91,51,93,91,51,93,0,97,108,102,97,0,114,97,100,105,117,115,0,105,112,111,0,104,49,0,104,50,0,102,51,0,104,105,100,101,0,101,97,115,105,110,103,0,98,97,99,107,0,112,101,114,105,111,100,0,97,117,116,111,95,104,97,110,100,108,101,95,116,121,112,101,0,118,101,99,91,52,93,0,95,112,97,100,49,91,49,93,0,109,97,116,95,110,114,0,112,110,116,115,117,0,112,110,116,115,118,0,114,101,115,111,108,117,0,114,101,115,
111,108,118,0,111,114,100,101,114,117,0,111,114,100,101,114,118,0,102,108,97,103,117,0,102,108,97,103,118,0,42,107,110,111,116,115,117,0,42,107,110,111,116,115,118,0,42,98,112,0,116,105,108,116,95,105,110,116,101,114,112,0,114,97,100,105,117,115,95,105,110,116,101,114,112,0,99,104,97,114,105,100,120,0,107,101,114,110,0,119,0,110,117,114,98,0,42,101,100,105,116,110,117,114,98,0,42,98,101,118,111,98,106,0,42,116,97,112,101,114,
111,98,106,0,42,116,101,120,116,111,110,99,117,114,118,101,0,42,107,101,121,0,42,42,109,97,116,0,42,98,101,118,101,108,95,112,114,111,102,105,108,101,0,116,101,120,102,108,97,103,0,116,119,105,115,116,95,109,111,100,101,0,116,119,105,115,116,95,115,109,111,111,116,104,0,115,109,97,108,108,99,97,112,115,95,115,99,97,108,101,0,112,97,116,104,108,101,110,0,98,101,118,114,101,115,111,108,0,116,111,116,99,111,108,0,119,105,100,116,104,
0,101,120,116,49,0,101,120,116,50,0,114,101,115,111,108,117,95,114,101,110,0,114,101,115,111,108,118,95,114,101,110,0,97,99,116,110,117,0,97,99,116,118,101,114,116,0,111,118,101,114,102,108,111,119,0,115,112,97,99,101,109,111,100,101,0,97,108,105,103,110,95,121,0,98,101,118,101,108,95,109,111,100,101,0,116,97,112,101,114,95,114,97,100,105,117,115,95,109,111,100,101,0,108,105,110,101,115,0,108,105,110,101,100,105,115,116,0,102,115,
105,122,101,0,119,111,114,100,115,112,97,99,101,0,117,108,112,111,115,0,117,108,104,101,105,103,104,116,0,120,111,102,0,121,111,102,0,108,105,110,101,119,105,100,116,104,0,115,101,108,115,116,97,114,116,0,115,101,108,101,110,100,0,108,101,110,95,119,99,104,97,114,0,42,115,116,114,0,42,101,100,105,116,102,111,110,116,0,102,97,109,105,108,121,91,54,52,93,0,42,118,102,111,110,116,0,42,118,102,111,110,116,98,0,42,118,102,111,110,116,
105,0,42,118,102,111,110,116,98,105,0,42,116,98,0,116,111,116,98,111,120,0,97,99,116,98,111,120,0,42,115,116,114,105,110,102,111,0,99,117,114,105,110,102,111,0,98,101,118,102,97,99,49,0,98,101,118,102,97,99,50,0,98,101,118,102,97,99,49,95,109,97,112,112,105,110,103,0,98,101,118,102,97,99,50,95,109,97,112,112,105,110,103,0,95,112,97,100,50,91,54,93,0,102,115,105,122,101,95,114,101,97,108,116,105,109,101,0,42,99,
117,114,118,101,95,101,118,97,108,0,101,100,105,116,95,100,97,116,97,95,102,114,111,109,95,111,114,105,103,105,110,97,108,0,95,112,97,100,51,91,55,93,0,42,98,97,116,99,104,95,99,97,99,104,101,0,104,49,95,108,111,99,91,50,93,0,104,50,95,108,111,99,91,50,93,0,42,112,114,111,102,105,108,101,0,112,97,116,104,95,108,101,110,0,115,101,103,109,101,110,116,115,95,108,101,110,0,42,115,101,103,109,101,110,116,115,0,118,105,101,
119,95,114,101,99,116,0,99,108,105,112,95,114,101,99,116,0,42,99,117,114,118,101,95,111,102,102,115,101,116,115,0,97,116,116,114,105,98,117,116,101,95,115,116,111,114,97,103,101,0,112,111,105,110,116,95,100,97,116,97,0,99,117,114,118,101,95,100,97,116,97,0,112,111,105,110,116,95,115,105,122,101,0,99,117,114,118,101,95,115,105,122,101,0,118,101,114,116,101,120,95,103,114,111,117,112,95,110,97,109,101,115,0,118,101,114,116,101,120,95,
103,114,111,117,112,95,97,99,116,105,118,101,95,105,110,100,101,120,0,97,116,116,114,105,98,117,116,101,115,95,97,99,116,105,118,101,95,105,110,100,101,120,0,42,99,117,115,116,111,109,95,107,110,111,116,115,0,99,117,115,116,111,109,95,107,110,111,116,95,110,117,109,0,103,101,111,109,101,116,114,121,0,115,121,109,109,101,116,114,121,0,115,101,108,101,99,116,105,111,110,95,100,111,109,97,105,110,0,42,115,117,114,102,97,99,101,0,42,115,117,
114,102,97,99,101,95,117,118,95,109,97,112,0,115,117,114,102,97,99,101,95,99,111,108,108,105,115,105,111,110,95,100,105,115,116,97,110,99,101,0,97,99,116,105,118,101,0,97,99,116,105,118,101,95,114,110,100,0,97,99,116,105,118,101,95,99,108,111,110,101,0,97,99,116,105,118,101,95,109,97,115,107,0,117,105,100,0,110,97,109,101,91,54,56,93,0,102,105,108,101,110,97,109,101,91,49,48,50,52,93,0,42,108,97,121,101,114,115,0,116,
121,112,101,109,97,112,91,53,51,93,0,116,111,116,108,97,121,101,114,0,109,97,120,108,97,121,101,114,0,116,111,116,115,105,122,101,0,42,112,111,111,108,0,42,101,120,116,101,114,110,97,108,0,118,109,97,115,107,0,101,109,97,115,107,0,102,109,97,115,107,0,112,109,97,115,107,0,108,109,97,115,107,0,42,99,97,110,118,97,115,0,42,98,114,117,115,104,95,103,114,111,117,112,0,42,112,111,105,110,116,99,97,99,104,101,0,112,116,99,97,
99,104,101,115,0,99,117,114,114,101,110,116,95,102,114,97,109,101,0,102,111,114,109,97,116,0,100,105,115,112,95,116,121,112,101,0,105,109,97,103,101,95,102,105,108,101,102,111,114,109,97,116,0,101,102,102,101,99,116,95,117,105,0,105,110,105,116,95,99,111,108,111,114,95,116,121,112,101,0,101,102,102,101,99,116,0,105,109,97,103,101,95,114,101,115,111,108,117,116,105,111,110,0,115,117,98,115,116,101,112,115,0,105,110,105,116,95,99,111,108,
111,114,91,52,93,0,42,105,110,105,116,95,116,101,120,116,117,114,101,0,105,110,105,116,95,108,97,121,101,114,110,97,109,101,91,54,56,93,0,100,114,121,95,115,112,101,101,100,0,100,105,115,115,95,115,112,101,101,100,0,99,111,108,111,114,95,100,114,121,95,116,104,114,101,115,104,111,108,100,0,100,101,112,116,104,95,99,108,97,109,112,0,100,105,115,112,95,102,97,99,116,111,114,0,115,112,114,101,97,100,95,115,112,101,101,100,0,99,111,108,
111,114,95,115,112,114,101,97,100,95,115,112,101,101,100,0,115,104,114,105,110,107,95,115,112,101,101,100,0,100,114,105,112,95,118,101,108,0,100,114,105,112,95,97,99,99,0,105,110,102,108,117,101,110,99,101,95,115,99,97,108,101,0,114,97,100,105,117,115,95,115,99,97,108,101,0,119,97,118,101,95,100,97,109,112,105,110,103,0,119,97,118,101,95,115,112,101,101,100,0,119,97,118,101,95,116,105,109,101,115,99,97,108,101,0,119,97,118,101,95,
115,112,114,105,110,103,0,119,97,118,101,95,115,109,111,111,116,104,110,101,115,115,0,117,118,108,97,121,101,114,95,110,97,109,101,91,54,56,93,0,105,109,97,103,101,95,111,117,116,112,117,116,95,112,97,116,104,91,49,48,50,52,93,0,111,117,116,112,117,116,95,110,97,109,101,91,54,56,93,0,111,117,116,112,117,116,95,110,97,109,101,50,91,54,56,93,0,42,112,109,100,0,115,117,114,102,97,99,101,115,0,97,99,116,105,118,101,95,115,117,
114,0,101,114,114,111,114,91,54,52,93,0,42,112,115,121,115,0,99,111,108,108,105,115,105,111,110,0,119,101,116,110,101,115,115,0,112,97,114,116,105,99,108,101,95,114,97,100,105,117,115,0,112,97,114,116,105,99,108,101,95,115,109,111,111,116,104,0,112,97,105,110,116,95,100,105,115,116,97,110,99,101,0,42,112,97,105,110,116,95,114,97,109,112,0,42,118,101,108,95,114,97,109,112,0,112,114,111,120,105,109,105,116,121,95,102,97,108,108,111,
102,102,0,119,97,118,101,95,116,121,112,101,0,114,97,121,95,100,105,114,0,119,97,118,101,95,102,97,99,116,111,114,0,119,97,118,101,95,99,108,97,109,112,0,109,97,120,95,118,101,108,111,99,105,116,121,0,115,109,117,100,103,101,95,115,116,114,101,110,103,116,104,0,98,117,116,116,121,112,101,0,115,116,121,112,101,0,118,101,114,116,103,114,111,117,112,0,117,115,101,114,106,105,116,0,115,116,97,0,108,105,102,101,116,105,109,101,0,116,111,
116,112,97,114,116,0,116,111,116,107,101,121,0,115,101,101,100,0,110,111,114,109,102,97,99,0,111,98,102,97,99,0,114,97,110,100,102,97,99,0,116,101,120,102,97,99,0,114,97,110,100,108,105,102,101,0,102,111,114,99,101,91,51,93,0,100,97,109,112,0,110,97,98,108,97,0,118,101,99,116,115,105,122,101,0,109,97,120,108,101,110,0,100,101,102,118,101,99,91,51,93,0,109,117,108,116,91,52,93,0,108,105,102,101,91,52,93,0,99,104,
105,108,100,91,52,93,0,109,97,116,91,52,93,0,116,101,120,109,97,112,0,99,117,114,109,117,108,116,0,115,116,97,116,105,99,115,116,101,112,0,111,109,97,116,0,116,105,109,101,116,101,120,0,115,112,101,101,100,116,101,120,0,102,108,97,103,50,110,101,103,0,100,105,115,112,0,118,101,114,116,103,114,111,117,112,95,118,0,118,103,114,111,117,112,110,97,109,101,91,54,52,93,0,118,103,114,111,117,112,110,97,109,101,95,118,91,54,52,93,0,
105,109,97,116,91,52,93,91,52,93,0,42,107,101,121,115,0,115,116,97,114,116,120,0,115,116,97,114,116,121,0,110,97,114,114,111,119,0,109,105,110,102,97,99,0,116,105,109,101,111,102,102,115,0,115,117,98,118,115,116,114,91,52,93,0,115,117,98,118,101,114,115,105,111,110,0,109,105,110,118,101,114,115,105,111,110,0,109,105,110,115,117,98,118,101,114,115,105,111,110,0,42,99,117,114,115,99,114,101,101,110,0,42,99,117,114,115,99,101,110,
101,0,42,99,117,114,95,118,105,101,119,95,108,97,121,101,114,0,102,105,108,101,102,108,97,103,115,0,103,108,111,98,97,108,102,0,98,117,105,108,100,95,99,111,109,109,105,116,95,116,105,109,101,115,116,97,109,112,0,98,117,105,108,100,95,104,97,115,104,91,49,54,93,0,42,102,109,100,0,42,102,108,117,105,100,0,42,102,108,117,105,100,95,111,108,100,0,42,102,108,117,105,100,95,109,117,116,101,120,0,42,102,108,117,105,100,95,103,114,111,
117,112,0,42,102,111,114,99,101,95,103,114,111,117,112,0,42,101,102,102,101,99,116,111,114,95,103,114,111,117,112,0,42,116,101,120,95,100,101,110,115,105,116,121,0,42,116,101,120,95,99,111,108,111,114,0,42,116,101,120,95,119,116,0,42,116,101,120,95,115,104,97,100,111,119,0,42,116,101,120,95,102,108,97,109,101,0,42,116,101,120,95,102,108,97,109,101,95,99,111,98,97,0,42,116,101,120,95,99,111,98,97,0,42,116,101,120,95,102,105,
101,108,100,0,42,116,101,120,95,118,101,108,111,99,105,116,121,95,120,0,42,116,101,120,95,118,101,108,111,99,105,116,121,95,121,0,42,116,101,120,95,118,101,108,111,99,105,116,121,95,122,0,42,116,101,120,95,102,108,97,103,115,0,42,116,101,120,95,114,97,110,103,101,95,102,105,101,108,100,0,42,103,117,105,100,105,110,103,95,112,97,114,101,110,116,0,112,48,91,51,93,0,112,49,91,51,93,0,100,112,48,91,51,93,0,99,101,108,108,95,
115,105,122,101,91,51,93,0,103,108,111,98,97,108,95,115,105,122,101,91,51,93,0,112,114,101,118,95,108,111,99,91,51,93,0,115,104,105,102,116,91,51,93,0,115,104,105,102,116,95,102,91,51,93,0,111,98,106,95,115,104,105,102,116,95,102,91,51,93,0,111,98,109,97,116,91,52,93,91,52,93,0,102,108,117,105,100,109,97,116,91,52,93,91,52,93,0,102,108,117,105,100,109,97,116,95,119,116,91,52,93,91,52,93,0,98,97,115,101,95,
114,101,115,91,51,93,0,114,101,115,95,109,105,110,91,51,93,0,114,101,115,95,109,97,120,91,51,93,0,114,101,115,91,51,93,0,116,111,116,97,108,95,99,101,108,108,115,0,100,120,0,98,111,117,110,100,97,114,121,95,119,105,100,116,104,0,103,114,97,118,105,116,121,95,102,105,110,97,108,91,51,93,0,97,100,97,112,116,95,109,97,114,103,105,110,0,97,100,97,112,116,95,114,101,115,0,97,100,97,112,116,95,116,104,114,101,115,104,111,108,
100,0,109,97,120,114,101,115,0,115,111,108,118,101,114,95,114,101,115,0,98,111,114,100,101,114,95,99,111,108,108,105,115,105,111,110,115,0,97,99,116,105,118,101,95,102,105,101,108,100,115,0,98,101,116,97,0,118,111,114,116,105,99,105,116,121,0,97,99,116,105,118,101,95,99,111,108,111,114,91,51,93,0,104,105,103,104,114,101,115,95,115,97,109,112,108,105,110,103,0,98,117,114,110,105,110,103,95,114,97,116,101,0,102,108,97,109,101,95,115,
109,111,107,101,0,102,108,97,109,101,95,118,111,114,116,105,99,105,116,121,0,102,108,97,109,101,95,105,103,110,105,116,105,111,110,0,102,108,97,109,101,95,109,97,120,95,116,101,109,112,0,102,108,97,109,101,95,115,109,111,107,101,95,99,111,108,111,114,91,51,93,0,110,111,105,115,101,95,115,116,114,101,110,103,116,104,0,110,111,105,115,101,95,112,111,115,95,115,99,97,108,101,0,110,111,105,115,101,95,116,105,109,101,95,97,110,105,109,0,114,
101,115,95,110,111,105,115,101,91,51,93,0,110,111,105,115,101,95,115,99,97,108,101,0,95,112,97,100,51,91,52,93,0,112,97,114,116,105,99,108,101,95,114,97,110,100,111,109,110,101,115,115,0,112,97,114,116,105,99,108,101,95,110,117,109,98,101,114,0,112,97,114,116,105,99,108,101,95,109,105,110,105,109,117,109,0,112,97,114,116,105,99,108,101,95,109,97,120,105,109,117,109,0,112,97,114,116,105,99,108,101,95,98,97,110,100,95,119,105,100,
116,104,0,102,114,97,99,116,105,111,110,115,95,116,104,114,101,115,104,111,108,100,0,102,114,97,99,116,105,111,110,115,95,100,105,115,116,97,110,99,101,0,102,108,105,112,95,114,97,116,105,111,0,115,121,115,95,112,97,114,116,105,99,108,101,95,109,97,120,105,109,117,109,0,115,105,109,117,108,97,116,105,111,110,95,109,101,116,104,111,100,0,95,112,97,100,52,91,54,93,0,118,105,115,99,111,115,105,116,121,95,118,97,108,117,101,0,95,112,97,
100,53,91,52,93,0,115,117,114,102,97,99,101,95,116,101,110,115,105,111,110,0,118,105,115,99,111,115,105,116,121,95,98,97,115,101,0,118,105,115,99,111,115,105,116,121,95,101,120,112,111,110,101,110,116,0,109,101,115,104,95,99,111,110,99,97,118,101,95,117,112,112,101,114,0,109,101,115,104,95,99,111,110,99,97,118,101,95,108,111,119,101,114,0,109,101,115,104,95,112,97,114,116,105,99,108,101,95,114,97,100,105,117,115,0,109,101,115,104,95,
115,109,111,111,116,104,101,110,95,112,111,115,0,109,101,115,104,95,115,109,111,111,116,104,101,110,95,110,101,103,0,109,101,115,104,95,115,99,97,108,101,0,109,101,115,104,95,103,101,110,101,114,97,116,111,114,0,95,112,97,100,54,91,50,93,0,112,97,114,116,105,99,108,101,95,116,121,112,101,0,112,97,114,116,105,99,108,101,95,115,99,97,108,101,0,115,110,100,112,97,114,116,105,99,108,101,95,116,97,117,95,109,105,110,95,119,99,0,115,110,
100,112,97,114,116,105,99,108,101,95,116,97,117,95,109,97,120,95,119,99,0,115,110,100,112,97,114,116,105,99,108,101,95,116,97,117,95,109,105,110,95,116,97,0,115,110,100,112,97,114,116,105,99,108,101,95,116,97,117,95,109,97,120,95,116,97,0,115,110,100,112,97,114,116,105,99,108,101,95,116,97,117,95,109,105,110,95,107,0,115,110,100,112,97,114,116,105,99,108,101,95,116,97,117,95,109,97,120,95,107,0,115,110,100,112,97,114,116,105,99,
108,101,95,107,95,119,99,0,115,110,100,112,97,114,116,105,99,108,101,95,107,95,116,97,0,115,110,100,112,97,114,116,105,99,108,101,95,107,95,98,0,115,110,100,112,97,114,116,105,99,108,101,95,107,95,100,0,115,110,100,112,97,114,116,105,99,108,101,95,108,95,109,105,110,0,115,110,100,112,97,114,116,105,99,108,101,95,108,95,109,97,120,0,115,110,100,112,97,114,116,105,99,108,101,95,112,111,116,101,110,116,105,97,108,95,114,97,100,105,117,
115,0,115,110,100,112,97,114,116,105,99,108,101,95,117,112,100,97,116,101,95,114,97,100,105,117,115,0,115,110,100,112,97,114,116,105,99,108,101,95,98,111,117,110,100,97,114,121,0,115,110,100,112,97,114,116,105,99,108,101,95,99,111,109,98,105,110,101,100,95,101,120,112,111,114,116,0,103,117,105,100,105,110,103,95,97,108,112,104,97,0,103,117,105,100,105,110,103,95,98,101,116,97,0,103,117,105,100,105,110,103,95,118,101,108,95,102,97,99,116,
111,114,0,103,117,105,100,101,95,114,101,115,91,51,93,0,103,117,105,100,105,110,103,95,115,111,117,114,99,101,0,95,112,97,100,56,91,50,93,0,99,97,99,104,101,95,102,114,97,109,101,95,115,116,97,114,116,0,99,97,99,104,101,95,102,114,97,109,101,95,101,110,100,0,99,97,99,104,101,95,102,114,97,109,101,95,112,97,117,115,101,95,100,97,116,97,0,99,97,99,104,101,95,102,114,97,109,101,95,112,97,117,115,101,95,110,111,105,115,101,
0,99,97,99,104,101,95,102,114,97,109,101,95,112,97,117,115,101,95,109,101,115,104,0,99,97,99,104,101,95,102,114,97,109,101,95,112,97,117,115,101,95,112,97,114,116,105,99,108,101,115,0,99,97,99,104,101,95,102,114,97,109,101,95,112,97,117,115,101,95,103,117,105,100,105,110,103,0,99,97,99,104,101,95,102,114,97,109,101,95,111,102,102,115,101,116,0,99,97,99,104,101,95,102,108,97,103,0,99,97,99,104,101,95,109,101,115,104,95,102,
111,114,109,97,116,0,99,97,99,104,101,95,100,97,116,97,95,102,111,114,109,97,116,0,99,97,99,104,101,95,112,97,114,116,105,99,108,101,95,102,111,114,109,97,116,0,99,97,99,104,101,95,110,111,105,115,101,95,102,111,114,109,97,116,0,99,97,99,104,101,95,100,105,114,101,99,116,111,114,121,91,49,48,50,52,93,0,99,97,99,104,101,95,116,121,112,101,0,99,97,99,104,101,95,105,100,91,52,93,0,95,112,97,100,57,91,50,93,0,116,
105,109,101,95,116,111,116,97,108,0,116,105,109,101,95,112,101,114,95,102,114,97,109,101,0,102,114,97,109,101,95,108,101,110,103,116,104,0,99,102,108,95,99,111,110,100,105,116,105,111,110,0,116,105,109,101,115,116,101,112,115,95,109,105,110,105,109,117,109,0,116,105,109,101,115,116,101,112,115,95,109,97,120,105,109,117,109,0,115,108,105,99,101,95,112,101,114,95,118,111,120,101,108,0,115,108,105,99,101,95,100,101,112,116,104,0,100,105,115,112,
108,97,121,95,116,104,105,99,107,110,101,115,115,0,103,114,105,100,95,115,99,97,108,101,0,42,99,111,98,97,0,118,101,99,116,111,114,95,115,99,97,108,101,0,103,114,105,100,108,105,110,101,115,95,108,111,119,101,114,95,98,111,117,110,100,0,103,114,105,100,108,105,110,101,115,95,117,112,112,101,114,95,98,111,117,110,100,0,103,114,105,100,108,105,110,101,115,95,114,97,110,103,101,95,99,111,108,111,114,91,52,93,0,97,120,105,115,95,115,108,
105,99,101,95,109,101,116,104,111,100,0,115,108,105,99,101,95,97,120,105,115,0,115,104,111,119,95,103,114,105,100,108,105,110,101,115,0,100,114,97,119,95,118,101,108,111,99,105,116,121,0,118,101,99,116,111,114,95,100,114,97,119,95,116,121,112,101,0,118,101,99,116,111,114,95,102,105,101,108,100,0,118,101,99,116,111,114,95,115,99,97,108,101,95,119,105,116,104,95,109,97,103,110,105,116,117,100,101,0,118,101,99,116,111,114,95,100,114,97,119,
95,109,97,99,95,99,111,109,112,111,110,101,110,116,115,0,117,115,101,95,99,111,98,97,0,99,111,98,97,95,102,105,101,108,100,0,105,110,116,101,114,112,95,109,101,116,104,111,100,0,103,114,105,100,108,105,110,101,115,95,99,111,108,111,114,95,102,105,101,108,100,0,103,114,105,100,108,105,110,101,115,95,99,101,108,108,95,102,105,108,116,101,114,0,95,112,97,100,49,48,91,51,93,0,118,101,108,111,99,105,116,121,95,115,99,97,108,101,0,111,
112,101,110,118,100,98,95,99,111,109,112,114,101,115,115,105,111,110,0,99,108,105,112,112,105,110,103,0,111,112,101,110,118,100,98,95,100,97,116,97,95,100,101,112,116,104,0,95,112,97,100,49,49,91,55,93,0,118,105,101,119,115,101,116,116,105,110,103,115,0,95,112,97,100,49,50,91,52,93,0,42,112,111,105,110,116,95,99,97,99,104,101,91,50,93,0,112,116,99,97,99,104,101,115,91,50,93,0,99,97,99,104,101,95,99,111,109,112,0,99,
97,99,104,101,95,104,105,103,104,95,99,111,109,112,0,99,97,99,104,101,95,102,105,108,101,95,102,111,114,109,97,116,0,95,112,97,100,49,51,91,55,93,0,42,109,101,115,104,0,42,110,111,105,115,101,95,116,101,120,116,117,114,101,0,42,118,101,114,116,115,95,111,108,100,0,110,117,109,118,101,114,116,115,0,118,101,108,95,109,117,108,116,105,0,118,101,108,95,110,111,114,109,97,108,0,118,101,108,95,114,97,110,100,111,109,0,118,101,108,95,
99,111,111,114,100,91,51,93,0,102,117,101,108,95,97,109,111,117,110,116,0,118,111,108,117,109,101,95,100,101,110,115,105,116,121,0,115,117,114,102,97,99,101,95,100,105,115,116,97,110,99,101,0,112,97,114,116,105,99,108,101,95,115,105,122,101,0,115,117,98,102,114,97,109,101,115,0,116,101,120,116,117,114,101,95,115,105,122,101,0,116,101,120,116,117,114,101,95,111,102,102,115,101,116,0,118,103,114,111,117,112,95,100,101,110,115,105,116,121,0,
98,101,104,97,118,105,111,114,0,116,101,120,116,117,114,101,95,116,121,112,101,0,95,112,97,100,52,91,51,93,0,103,117,105,100,105,110,103,95,109,111,100,101,0,115,101,108,101,99,116,105,111,110,0,113,105,0,113,105,95,115,116,97,114,116,0,113,105,95,101,110,100,0,101,100,103,101,95,116,121,112,101,115,0,101,120,99,108,117,100,101,95,101,100,103,101,95,116,121,112,101,115,0,42,108,105,110,101,115,116,121,108,101,0,42,115,99,114,105,112,
116,0,105,115,95,100,105,115,112,108,97,121,101,100,0,109,111,100,117,108,101,115,0,114,97,121,99,97,115,116,105,110,103,95,97,108,103,111,114,105,116,104,109,0,115,112,104,101,114,101,95,114,97,100,105,117,115,0,100,107,114,95,101,112,115,105,108,111,110,0,99,114,101,97,115,101,95,97,110,103,108,101,0,108,105,110,101,115,101,116,115,0,122,0,117,118,95,102,97,99,0,117,118,95,114,111,116,0,117,118,95,102,105,108,108,91,50,93,0,118,
101,114,116,95,99,111,108,111,114,91,52,93,0,118,101,114,116,115,91,51,93,0,105,110,102,111,91,54,52,93,0,99,111,108,111,114,91,52,93,0,102,105,108,108,91,52,93,0,98,101,122,116,0,112,111,105,110,116,95,105,110,100,101,120,0,42,99,117,114,118,101,95,112,111,105,110,116,115,0,116,111,116,95,99,117,114,118,101,95,112,111,105,110,116,115,0,116,109,112,95,108,97,121,101,114,105,110,102,111,91,49,50,56,93,0,109,117,108,116,105,
95,102,114,97,109,101,95,102,97,108,108,111,102,102,0,115,116,114,111,107,101,95,115,116,97,114,116,0,102,105,108,108,95,115,116,97,114,116,0,118,101,114,116,101,120,95,115,116,97,114,116,0,99,117,114,118,101,95,115,116,97,114,116,0,42,103,112,115,95,111,114,105,103,0,42,116,114,105,97,110,103,108,101,115,0,116,111,116,112,111,105,110,116,115,0,116,111,116,95,116,114,105,97,110,103,108,101,115,0,116,104,105,99,107,110,101,115,115,0,105,
110,105,116,116,105,109,101,0,99,111,108,111,114,110,97,109,101,91,49,50,56,93,0,99,97,112,115,91,50,93,0,102,105,108,108,95,111,112,97,99,105,116,121,95,102,97,99,0,117,118,95,114,111,116,97,116,105,111,110,0,117,118,95,116,114,97,110,115,108,97,116,105,111,110,91,50,93,0,117,118,95,115,99,97,108,101,0,115,101,108,101,99,116,95,105,110,100,101,120,0,95,112,97,100,52,91,52,93,0,42,100,118,101,114,116,0,118,101,114,116,
95,99,111,108,111,114,95,102,105,108,108,91,52,93,0,42,101,100,105,116,99,117,114,118,101,0,42,95,112,97,100,53,0,102,114,97,109,101,105,100,0,111,110,105,111,110,95,105,100,0,115,116,114,111,107,101,115,0,102,114,97,109,101,110,117,109,0,107,101,121,95,116,121,112,101,0,110,97,109,101,91,49,50,56,93,0,115,111,114,116,95,105,110,100,101,120,0,102,114,97,109,101,115,0,42,97,99,116,102,114,97,109,101,0,111,110,105,111,110,95,
102,108,97,103,0,105,110,102,111,91,49,50,56,93,0,112,97,115,115,95,105,110,100,101,120,0,105,110,118,101,114,115,101,91,52,93,91,52,93,0,112,97,114,115,117,98,115,116,114,91,54,52,93,0,112,97,114,116,121,112,101,0,108,105,110,101,95,99,104,97,110,103,101,0,116,105,110,116,99,111,108,111,114,91,52,93,0,111,112,97,99,105,116,121,0,118,105,101,119,108,97,121,101,114,110,97,109,101,91,54,52,93,0,98,108,101,110,100,95,109,
111,100,101,0,118,101,114,116,101,120,95,112,97,105,110,116,95,111,112,97,99,105,116,121,0,103,115,116,101,112,0,103,115,116,101,112,95,110,101,120,116,0,103,99,111,108,111,114,95,112,114,101,118,91,51,93,0,103,99,111,108,111,114,95,110,101,120,116,91,51,93,0,109,97,115,107,95,108,97,121,101,114,115,0,97,99,116,95,109,97,115,107,0,108,111,99,97,116,105,111,110,91,51,93,0,114,111,116,97,116,105,111,110,91,51,93,0,115,99,97,
108,101,91,51,93,0,108,97,121,101,114,95,109,97,116,91,52,93,91,52,93,0,108,97,121,101,114,95,105,110,118,109,97,116,91,52,93,91,52,93,0,42,115,98,117,102,102,101,114,0,112,108,97,121,105,110,103,0,109,97,116,105,100,0,115,98,117,102,102,101,114,95,115,102,108,97,103,0,115,98,117,102,102,101,114,95,117,115,101,100,0,115,98,117,102,102,101,114,95,115,105,122,101,0,97,114,114,111,119,95,115,116,97,114,116,91,56,93,0,97,
114,114,111,119,95,101,110,100,91,56,93,0,97,114,114,111,119,95,115,116,97,114,116,95,115,116,121,108,101,0,97,114,114,111,119,95,101,110,100,95,115,116,121,108,101,0,115,99,97,108,101,91,50,93,0,99,117,114,118,101,95,101,100,105,116,95,114,101,115,111,108,117,116,105,111,110,0,99,117,114,118,101,95,101,100,105,116,95,116,104,114,101,115,104,111,108,100,0,99,117,114,118,101,95,101,100,105,116,95,99,111,114,110,101,114,95,97,110,103,108,
101,0,112,97,108,101,116,116,101,115,0,112,105,120,102,97,99,116,111,114,0,108,105,110,101,95,99,111,108,111,114,91,52,93,0,111,110,105,111,110,95,102,97,99,116,111,114,0,111,110,105,111,110,95,109,111,100,101,0,122,100,101,112,116,104,95,111,102,102,115,101,116,0,116,111,116,102,114,97,109,101,0,116,111,116,115,116,114,111,107,101,0,100,114,97,119,95,109,111,100,101,0,111,110,105,111,110,95,107,101,121,116,121,112,101,0,115,101,108,101,
99,116,95,108,97,115,116,95,105,110,100,101,120,0,103,114,105,100,0,42,101,114,114,111,114,0,109,111,100,105,102,105,101,114,0,108,97,121,101,114,110,97,109,101,91,54,52,93,0,109,97,116,101,114,105,97,108,110,97,109,101,91,54,52,93,0,118,103,110,97,109,101,91,54,52,93,0,102,97,99,116,111,114,0,102,97,99,116,111,114,95,115,116,114,101,110,103,116,104,0,102,97,99,116,111,114,95,116,104,105,99,107,110,101,115,115,0,102,97,99,
116,111,114,95,117,118,115,0,110,111,105,115,101,95,111,102,102,115,101,116,0,110,111,105,115,101,95,109,111,100,101,0,108,97,121,101,114,95,112,97,115,115,0,42,99,117,114,118,101,95,105,110,116,101,110,115,105,116,121,0,116,104,105,99,107,110,101,115,115,95,102,97,99,0,42,99,117,114,118,101,95,116,104,105,99,107,110,101,115,115,0,42,103,112,109,100,0,115,101,103,95,115,116,97,114,116,0,115,101,103,95,101,110,100,0,115,101,103,95,109,
111,100,101,0,115,101,103,95,114,101,112,101,97,116,0,102,114,97,109,101,95,115,99,97,108,101,0,115,101,103,109,101,110,116,95,97,99,116,105,118,101,95,105,110,100,101,120,0,104,115,118,91,51,93,0,109,111,100,105,102,121,95,99,111,108,111,114,0,104,97,114,100,101,110,101,115,115,0,42,111,98,106,101,99,116,0,115,97,109,112,108,101,95,108,101,110,103,116,104,0,115,117,98,100,105,118,0,42,111,117,116,108,105,110,101,95,109,97,116,101,
114,105,97,108,0,99,111,117,110,116,0,114,110,100,95,111,102,102,115,101,116,91,51,93,0,114,110,100,95,114,111,116,91,51,93,0,114,110,100,95,115,99,97,108,101,91,51,93,0,109,97,116,95,114,112,108,0,115,116,97,114,116,95,100,101,108,97,121,0,116,114,97,110,115,105,116,105,111,110,0,116,105,109,101,95,97,108,105,103,110,109,101,110,116,0,115,112,101,101,100,95,102,97,99,0,115,112,101,101,100,95,109,97,120,103,97,112,0,116,105,
109,101,95,109,111,100,101,0,112,101,114,99,101,110,116,97,103,101,95,102,97,99,0,102,97,100,101,95,102,97,99,0,116,97,114,103,101,116,95,118,103,110,97,109,101,91,54,52,93,0,102,97,100,101,95,111,112,97,99,105,116,121,95,115,116,114,101,110,103,116,104,0,102,97,100,101,95,116,104,105,99,107,110,101,115,115,95,115,116,114,101,110,103,116,104,0,42,99,97,99,104,101,95,100,97,116,97,0,115,116,97,114,116,95,102,97,99,0,101,110,
100,95,102,97,99,0,114,97,110,100,95,115,116,97,114,116,95,102,97,99,0,114,97,110,100,95,101,110,100,95,102,97,99,0,114,97,110,100,95,111,102,102,115,101,116,0,111,118,101,114,115,104,111,111,116,95,102,97,99,0,112,111,105,110,116,95,100,101,110,115,105,116,121,0,115,101,103,109,101,110,116,95,105,110,102,108,117,101,110,99,101,0,109,97,120,95,97,110,103,108,101,0,42,100,109,100,0,100,97,115,104,0,103,97,112,0,100,97,115,104,
95,111,102,102,115,101,116,0,102,97,108,108,111,102,102,95,116,121,112,101,0,112,97,114,101,110,116,105,110,118,91,52,93,91,52,93,0,99,101,110,116,91,51,93,0,102,111,114,99,101,0,42,99,117,114,102,97,108,108,111,102,102,0,114,111,116,91,51,93,0,115,116,114,111,107,101,95,115,116,101,112,0,115,116,114,111,107,101,95,115,116,97,114,116,95,111,102,102,115,101,116,0,109,117,108,116,105,0,40,42,118,101,114,116,95,99,111,111,114,100,
115,95,112,114,101,118,41,40,41,0,100,117,112,108,105,99,97,116,105,111,110,115,0,102,97,100,105,110,103,95,99,101,110,116,101,114,0,102,97,100,105,110,103,95,116,104,105,99,107,110,101,115,115,0,102,97,100,105,110,103,95,111,112,97,99,105,116,121,0,42,99,111,108,111,114,98,97,110,100,0,117,118,95,111,102,102,115,101,116,0,102,105,108,108,95,114,111,116,97,116,105,111,110,0,102,105,108,108,95,111,102,102,115,101,116,91,50,93,0,102,
105,108,108,95,115,99,97,108,101,0,102,105,116,95,109,101,116,104,111,100,0,97,108,105,103,110,109,101,110,116,95,114,111,116,97,116,105,111,110,0,109,105,110,95,119,101,105,103,104,116,0,100,105,115,116,95,115,116,97,114,116,0,100,105,115,116,95,101,110,100,0,97,120,105,115,0,97,110,103,108,101,0,108,105,110,101,95,116,121,112,101,115,0,115,111,117,114,99,101,95,116,121,112,101,0,117,115,101,95,109,117,108,116,105,112,108,101,95,108,101,
118,101,108,115,0,108,101,118,101,108,95,115,116,97,114,116,0,108,101,118,101,108,95,101,110,100,0,42,115,111,117,114,99,101,95,99,97,109,101,114,97,0,42,108,105,103,104,116,95,99,111,110,116,111,117,114,95,111,98,106,101,99,116,0,42,115,111,117,114,99,101,95,111,98,106,101,99,116,0,42,115,111,117,114,99,101,95,99,111,108,108,101,99,116,105,111,110,0,42,116,97,114,103,101,116,95,109,97,116,101,114,105,97,108,0,116,97,114,103,101,
116,95,108,97,121,101,114,91,54,52,93,0,115,111,117,114,99,101,95,118,101,114,116,101,120,95,103,114,111,117,112,91,54,52,93,0,111,118,101,114,115,99,97,110,0,115,104,97,100,111,119,95,99,97,109,101,114,97,95,102,111,118,0,115,104,97,100,111,119,95,99,97,109,101,114,97,95,115,105,122,101,0,115,104,97,100,111,119,95,99,97,109,101,114,97,95,110,101,97,114,0,115,104,97,100,111,119,95,99,97,109,101,114,97,95,102,97,114,0,116,
114,97,110,115,112,97,114,101,110,99,121,95,102,108,97,103,115,0,116,114,97,110,115,112,97,114,101,110,99,121,95,109,97,115,107,0,105,110,116,101,114,115,101,99,116,105,111,110,95,109,97,115,107,0,115,104,97,100,111,119,95,115,101,108,101,99,116,105,111,110,0,115,105,108,104,111,117,101,116,116,101,95,115,101,108,101,99,116,105,111,110,0,99,114,101,97,115,101,95,116,104,114,101,115,104,111,108,100,0,97,110,103,108,101,95,115,112,108,105,116,
116,105,110,103,95,116,104,114,101,115,104,111,108,100,0,99,104,97,105,110,95,115,109,111,111,116,104,95,116,111,108,101,114,97,110,99,101,0,99,104,97,105,110,105,110,103,95,105,109,97,103,101,95,116,104,114,101,115,104,111,108,100,0,99,97,108,99,117,108,97,116,105,111,110,95,102,108,97,103,115,0,115,116,114,111,107,101,95,100,101,112,116,104,95,111,102,102,115,101,116,0,108,101,118,101,108,95,115,116,97,114,116,95,111,118,101,114,114,105,100,
101,0,108,101,118,101,108,95,101,110,100,95,111,118,101,114,114,105,100,101,0,101,100,103,101,95,116,121,112,101,115,95,111,118,101,114,114,105,100,101,0,115,104,97,100,111,119,95,115,101,108,101,99,116,105,111,110,95,111,118,101,114,114,105,100,101,0,115,104,97,100,111,119,95,117,115,101,95,115,105,108,104,111,117,101,116,116,101,95,111,118,101,114,114,105,100,101,0,42,108,97,95,100,97,116,97,95,112,116,114,0,42,97,117,120,95,116,97,114,103,
101,116,0,107,101,101,112,95,100,105,115,116,0,115,104,114,105,110,107,95,116,121,112,101,0,115,104,114,105,110,107,95,111,112,116,115,0,115,104,114,105,110,107,95,109,111,100,101,0,112,114,111,106,95,108,105,109,105,116,0,112,114,111,106,95,97,120,105,115,0,115,117,98,115,117,114,102,95,108,101,118,101,108,115,0,115,109,111,111,116,104,95,102,97,99,116,111,114,0,115,109,111,111,116,104,95,115,116,101,112,0,115,107,105,112,0,115,112,114,101,
97,100,0,102,115,116,111,112,0,102,111,99,97,108,95,108,101,110,103,116,104,0,115,101,110,115,111,114,0,114,97,116,105,111,0,110,117,109,95,98,108,97,100,101,115,0,104,105,103,104,95,113,117,97,108,105,116,121,0,42,105,100,95,114,101,102,101,114,101,110,99,101,0,100,114,97,119,105,110,103,95,105,110,100,101,120,0,42,118,97,108,117,101,115,0,42,108,97,121,101,114,95,110,97,109,101,0,102,114,97,109,101,115,95,115,116,111,114,97,103,
101,0,109,97,115,107,115,0,97,99,116,105,118,101,95,109,97,115,107,95,105,110,100,101,120,0,42,112,97,114,115,117,98,115,116,114,0,116,114,97,110,115,108,97,116,105,111,110,91,51,93,0,42,118,105,101,119,108,97,121,101,114,110,97,109,101,0,102,105,108,116,101,114,0,110,117,109,95,102,114,97,109,101,115,95,98,101,102,111,114,101,0,110,117,109,95,102,114,97,109,101,115,95,97,102,116,101,114,0,99,111,108,111,114,95,98,101,102,111,114,
101,91,51,93,0,99,111,108,111,114,95,97,102,116,101,114,91,51,93,0,42,42,100,114,97,119,105,110,103,95,97,114,114,97,121,0,100,114,97,119,105,110,103,95,97,114,114,97,121,95,115,105,122,101,0,42,114,111,111,116,95,103,114,111,117,112,95,112,116,114,0,108,97,121,101,114,115,95,100,97,116,97,0,42,97,99,116,105,118,101,95,110,111,100,101,0,42,42,109,97,116,101,114,105,97,108,95,97,114,114,97,121,0,109,97,116,101,114,105,97,
108,95,97,114,114,97,121,95,115,105,122,101,0,95,112,97,100,51,91,50,93,0,111,110,105,111,110,95,115,107,105,110,110,105,110,103,95,115,101,116,116,105,110,103,115,0,42,115,99,101,110,101,0,102,114,97,109,101,110,114,0,99,121,99,108,0,109,117,108,116,105,118,105,101,119,95,101,121,101,0,112,97,115,115,0,116,105,108,101,0,109,117,108,116,105,95,105,110,100,101,120,0,118,105,101,119,0,42,97,110,105,109,0,116,105,108,101,95,110,117,
109,98,101,114,0,42,114,101,110,100,101,114,0,116,105,108,101,97,114,114,97,121,95,108,97,121,101,114,0,116,105,108,101,97,114,114,97,121,95,111,102,102,115,101,116,91,50,93,0,116,105,108,101,97,114,114,97,121,95,115,105,122,101,91,50,93,0,103,101,110,95,120,0,103,101,110,95,121,0,103,101,110,95,116,121,112,101,0,103,101,110,95,102,108,97,103,0,103,101,110,95,100,101,112,116,104,0,103,101,110,95,99,111,108,111,114,91,52,93,0,
108,97,98,101,108,91,54,52,93,0,42,103,112,117,116,101,120,116,117,114,101,91,51,93,91,50,93,0,97,110,105,109,115,0,42,114,114,0,114,101,110,100,101,114,115,108,111,116,115,0,114,101,110,100,101,114,95,115,108,111,116,0,108,97,115,116,95,114,101,110,100,101,114,95,115,108,111,116,0,108,97,115,116,102,114,97,109,101,0,103,112,117,102,114,97,109,101,110,114,0,103,112,117,102,108,97,103,0,103,112,117,95,112,97,115,115,0,103,112,117,
95,108,97,121,101,114,0,103,112,117,95,118,105,101,119,0,115,101,97,109,95,109,97,114,103,105,110,0,112,97,99,107,101,100,102,105,108,101,115,0,108,97,115,116,117,115,101,100,0,97,115,112,120,0,97,115,112,121,0,99,111,108,111,114,115,112,97,99,101,95,115,101,116,116,105,110,103,115,0,97,108,112,104,97,95,109,111,100,101,0,101,121,101,0,118,105,101,119,115,95,102,111,114,109,97,116,0,97,99,116,105,118,101,95,116,105,108,101,95,105,
110,100,101,120,0,116,105,108,101,115,0,118,105,101,119,115,0,42,115,116,101,114,101,111,51,100,95,102,111,114,109,97,116,0,98,108,111,99,107,116,121,112,101,0,97,100,114,99,111,100,101,0,109,97,120,114,99,116,0,116,111,116,114,99,116,0,118,97,114,116,121,112,101,0,101,120,116,114,97,112,0,98,105,116,109,97,115,107,0,115,108,105,100,101,95,109,105,110,0,115,108,105,100,101,95,109,97,120,0,99,117,114,118,101,0,115,104,111,119,107,
101,121,0,109,117,116,101,105,112,111,0,114,101,108,97,116,105,118,101,0,116,111,116,101,108,101,109,0,118,103,114,111,117,112,91,54,52,93,0,115,108,105,100,101,114,109,105,110,0,115,108,105,100,101,114,109,97,120,0,42,114,101,102,107,101,121,0,101,108,101,109,115,116,114,91,51,50,93,0,101,108,101,109,115,105,122,101,0,98,108,111,99,107,0,42,102,114,111,109,0,117,105,100,103,101,110,0,112,110,116,115,119,0,111,112,110,116,115,117,0,
111,112,110,116,115,118,0,111,112,110,116,115,119,0,116,121,112,101,117,0,116,121,112,101,118,0,116,121,112,101,119,0,97,99,116,98,112,0,102,117,0,102,118,0,102,119,0,100,117,0,100,118,0,100,119,0,42,100,101,102,0,42,101,100,105,116,108,97,116,116,0,42,98,97,115,101,95,111,114,105,103,0,108,97,121,0,102,108,97,103,95,102,114,111,109,95,99,111,108,108,101,99,116,105,111,110,0,102,108,97,103,95,108,101,103,97,99,121,0,108,
111,99,97,108,95,118,105,101,119,95,98,105,116,115,0,108,111,99,97,108,95,99,111,108,108,101,99,116,105,111,110,115,95,98,105,116,115,0,114,117,110,116,105,109,101,95,102,108,97,103,0,108,97,121,101,114,95,99,111,108,108,101,99,116,105,111,110,115,0,114,101,110,100,101,114,95,112,97,115,115,101,115,0,111,98,106,101,99,116,95,98,97,115,101,115,0,42,115,116,97,116,115,0,42,98,97,115,97,99,116,0,108,97,121,102,108,97,103,0,112,
97,115,115,102,108,97,103,0,112,97,115,115,95,97,108,112,104,97,95,116,104,114,101,115,104,111,108,100,0,99,114,121,112,116,111,109,97,116,116,101,95,102,108,97,103,0,99,114,121,112,116,111,109,97,116,116,101,95,108,101,118,101,108,115,0,103,114,101,97,115,101,95,112,101,110,99,105,108,95,102,108,97,103,115,0,115,97,109,112,108,101,115,0,42,109,97,116,95,111,118,101,114,114,105,100,101,0,42,119,111,114,108,100,95,111,118,101,114,114,105,
100,101,0,42,105,100,95,112,114,111,112,101,114,116,105,101,115,0,102,114,101,101,115,116,121,108,101,95,99,111,110,102,105,103,0,101,101,118,101,101,0,97,111,118,115,0,42,97,99,116,105,118,101,95,97,111,118,0,108,105,103,104,116,103,114,111,117,112,115,0,42,97,99,116,105,118,101,95,108,105,103,104,116,103,114,111,117,112,0,42,42,111,98,106,101,99,116,95,98,97,115,101,115,95,97,114,114,97,121,0,42,111,98,106,101,99,116,95,98,97,
115,101,115,95,104,97,115,104,0,101,110,101,114,103,121,95,110,101,119,0,115,112,111,116,115,105,122,101,0,115,112,111,116,98,108,101,110,100,0,97,114,101,97,95,115,104,97,112,101,0,95,112,97,100,49,0,97,114,101,97,95,115,105,122,101,0,97,114,101,97,95,115,105,122,101,121,0,97,114,101,97,95,115,105,122,101,122,0,97,114,101,97,95,115,112,114,101,97,100,0,115,117,110,95,97,110,103,108,101,0,112,114,95,116,101,120,116,117,114,101,
0,117,115,101,95,110,111,100,101,115,0,99,97,115,99,97,100,101,95,109,97,120,95,100,105,115,116,0,99,97,115,99,97,100,101,95,101,120,112,111,110,101,110,116,0,99,97,115,99,97,100,101,95,102,97,100,101,0,99,97,115,99,97,100,101,95,99,111,117,110,116,0,100,105,102,102,95,102,97,99,0,115,112,101,99,95,102,97,99,0,116,114,97,110,115,109,105,115,115,105,111,110,95,102,97,99,0,118,111,108,117,109,101,95,102,97,99,0,97,116,
116,95,100,105,115,116,0,115,104,97,100,111,119,95,102,105,108,116,101,114,95,114,97,100,105,117,115,0,115,104,97,100,111,119,95,109,97,120,105,109,117,109,95,114,101,115,111,108,117,116,105,111,110,0,115,104,97,100,111,119,95,106,105,116,116,101,114,95,111,118,101,114,98,108,117,114,0,42,110,111,100,101,116,114,101,101,0,101,110,101,114,103,121,0,97,116,116,101,110,117,97,116,105,111,110,95,116,121,112,101,0,112,97,114,97,108,108,97,120,95,
116,121,112,101,0,103,114,105,100,95,102,108,97,103,0,100,105,115,116,105,110,102,0,100,105,115,116,112,97,114,0,118,105,115,95,98,105,97,115,0,118,105,115,95,98,108,101,101,100,98,105,97,115,0,118,105,115,95,98,108,117,114,0,105,110,116,101,110,115,105,116,121,0,103,114,105,100,95,114,101,115,111,108,117,116,105,111,110,95,120,0,103,114,105,100,95,114,101,115,111,108,117,116,105,111,110,95,121,0,103,114,105,100,95,114,101,115,111,108,117,
116,105,111,110,95,122,0,103,114,105,100,95,98,97,107,101,95,115,97,109,112,108,101,115,0,103,114,105,100,95,115,117,114,102,97,99,101,95,98,105,97,115,0,103,114,105,100,95,101,115,99,97,112,101,95,98,105,97,115,0,103,114,105,100,95,110,111,114,109,97,108,95,98,105,97,115,0,103,114,105,100,95,118,105,101,119,95,98,105,97,115,0,103,114,105,100,95,102,97,99,105,110,103,95,98,105,97,115,0,103,114,105,100,95,118,97,108,105,100,105,
116,121,95,116,104,114,101,115,104,111,108,100,0,103,114,105,100,95,100,105,108,97,116,105,111,110,95,116,104,114,101,115,104,111,108,100,0,103,114,105,100,95,100,105,108,97,116,105,111,110,95,114,97,100,105,117,115,0,103,114,105,100,95,99,108,97,109,112,95,100,105,114,101,99,116,0,103,114,105,100,95,99,108,97,109,112,95,105,110,100,105,114,101,99,116,0,103,114,105,100,95,115,117,114,102,101,108,95,100,101,110,115,105,116,121,0,42,118,105,115,
105,98,105,108,105,116,121,95,103,114,112,0,100,97,116,97,95,100,105,115,112,108,97,121,95,115,105,122,101,0,112,111,115,105,116,105,111,110,91,51,93,0,97,116,116,101,110,117,97,116,105,111,110,95,102,97,99,0,97,116,116,101,110,117,97,116,105,111,110,109,97,116,91,52,93,91,52,93,0,112,97,114,97,108,108,97,120,109,97,116,91,52,93,91,52,93,0,109,97,116,91,52,93,91,52,93,0,114,101,115,111,108,117,116,105,111,110,91,51,93,
0,99,111,114,110,101,114,91,51,93,0,97,116,116,101,110,117,97,116,105,111,110,95,115,99,97,108,101,0,105,110,99,114,101,109,101,110,116,95,120,91,51,93,0,97,116,116,101,110,117,97,116,105,111,110,95,98,105,97,115,0,105,110,99,114,101,109,101,110,116,95,121,91,51,93,0,108,101,118,101,108,95,98,105,97,115,0,105,110,99,114,101,109,101,110,116,95,122,91,51,93,0,95,112,97,100,52,0,118,105,115,105,98,105,108,105,116,121,95,98,
105,97,115,0,118,105,115,105,98,105,108,105,116,121,95,98,108,101,101,100,0,118,105,115,105,98,105,108,105,116,121,95,114,97,110,103,101,0,95,112,97,100,53,0,42,116,101,120,0,116,101,120,95,115,105,122,101,91,51,93,0,99,111,109,112,111,110,101,110,116,115,0,118,101,114,115,105,111,110,0,99,117,98,101,95,108,101,110,0,103,114,105,100,95,108,101,110,0,109,105,112,115,95,108,101,110,0,118,105,115,95,114,101,115,0,114,101,102,95,114,
101,115,0,95,112,97,100,91,52,93,91,50,93,0,103,114,105,100,95,116,120,0,99,117,98,101,95,116,120,0,42,99,117,98,101,95,109,105,112,115,0,42,99,117,98,101,95,100,97,116,97,0,42,103,114,105,100,95,100,97,116,97,0,40,42,76,48,41,40,41,0,40,42,76,49,95,97,41,40,41,0,40,42,76,49,95,98,41,40,41,0,40,42,76,49,95,99,41,40,41,0,42,118,97,108,105,100,105,116,121,0,40,42,118,105,114,116,117,97,108,
95,111,102,102,115,101,116,41,40,41,0,42,76,48,0,42,76,49,95,97,0,42,76,49,95,98,0,42,76,49,95,99,0,100,97,116,97,95,108,97,121,111,117,116,0,98,108,111,99,107,95,108,101,110,0,98,108,111,99,107,95,115,105,122,101,0,42,98,108,111,99,107,95,105,110,102,111,115,0,98,97,107,105,110,103,0,105,114,114,97,100,105,97,110,99,101,0,118,105,115,105,98,105,108,105,116,121,0,99,111,110,110,101,99,116,105,118,105,116,121,
0,115,117,114,102,101,108,115,95,108,101,110,0,42,115,117,114,102,101,108,115,0,115,104,97,114,101,100,0,100,105,114,116,121,0,42,103,114,105,100,95,115,116,97,116,105,99,95,99,97,99,104,101,0,42,99,111,108,111,114,95,114,97,109,112,0,118,97,108,117,101,95,109,105,110,0,118,97,108,117,101,95,109,97,120,0,114,97,110,103,101,95,109,105,110,0,114,97,110,103,101,95,109,97,120,0,109,105,110,95,99,117,114,118,97,116,117,114,101,0,
109,97,120,95,99,117,114,118,97,116,117,114,101,0,109,105,110,95,116,104,105,99,107,110,101,115,115,0,109,97,120,95,116,104,105,99,107,110,101,115,115,0,109,105,110,95,97,110,103,108,101,0,109,97,116,95,97,116,116,114,0,115,97,109,112,108,105,110,103,0,101,114,114,111,114,0,119,97,118,101,108,101,110,103,116,104,0,111,99,116,97,118,101,115,0,102,114,101,113,117,101,110,99,121,0,98,97,99,107,98,111,110,101,95,108,101,110,103,116,104,
0,116,105,112,95,108,101,110,103,116,104,0,114,111,117,110,100,115,0,114,97,110,100,111,109,95,114,97,100,105,117,115,0,114,97,110,100,111,109,95,99,101,110,116,101,114,0,114,97,110,100,111,109,95,98,97,99,107,98,111,110,101,0,115,99,97,108,101,95,120,0,115,99,97,108,101,95,121,0,112,105,118,111,116,95,117,0,112,105,118,111,116,95,120,0,112,105,118,111,116,95,121,0,116,111,108,101,114,97,110,99,101,0,111,114,105,101,110,116,97,
116,105,111,110,0,116,104,105,99,107,110,101,115,115,95,112,111,115,105,116,105,111,110,0,116,104,105,99,107,110,101,115,115,95,114,97,116,105,111,0,99,97,112,115,0,99,104,97,105,110,105,110,103,0,115,112,108,105,116,95,108,101,110,103,116,104,0,109,105,110,95,108,101,110,103,116,104,0,109,97,120,95,108,101,110,103,116,104,0,99,104,97,105,110,95,99,111,117,110,116,0,115,112,108,105,116,95,100,97,115,104,49,0,115,112,108,105,116,95,103,
97,112,49,0,115,112,108,105,116,95,100,97,115,104,50,0,115,112,108,105,116,95,103,97,112,50,0,115,112,108,105,116,95,100,97,115,104,51,0,115,112,108,105,116,95,103,97,112,51,0,115,111,114,116,95,107,101,121,0,105,110,116,101,103,114,97,116,105,111,110,95,116,121,112,101,0,116,101,120,115,116,101,112,0,116,101,120,97,99,116,0,100,97,115,104,49,0,103,97,112,49,0,100,97,115,104,50,0,103,97,112,50,0,100,97,115,104,51,0,103,
97,112,51,0,112,97,110,101,108,0,42,109,116,101,120,91,49,56,93,0,99,111,108,111,114,95,109,111,100,105,102,105,101,114,115,0,97,108,112,104,97,95,109,111,100,105,102,105,101,114,115,0,116,104,105,99,107,110,101,115,115,95,109,111,100,105,102,105,101,114,115,0,103,101,111,109,101,116,114,121,95,109,111,100,105,102,105,101,114,115,0,42,102,105,114,115,116,0,42,108,97,115,116,0,108,97,115,116,95,117,112,100,97,116,101,0,109,97,115,107,
108,97,121,101,114,115,0,109,97,115,107,108,97,121,95,97,99,116,0,109,97,115,107,108,97,121,95,116,111,116,0,112,97,114,101,110,116,91,54,52,93,0,115,117,98,95,112,97,114,101,110,116,91,54,52,93,0,112,97,114,101,110,116,95,111,114,105,103,91,50,93,0,112,97,114,101,110,116,95,99,111,114,110,101,114,115,95,111,114,105,103,91,52,93,91,50,93,0,117,0,116,111,116,95,117,119,0,42,117,119,0,112,97,114,101,110,116,0,111,102,
102,115,101,116,95,109,111,100,101,0,119,101,105,103,104,116,95,105,110,116,101,114,112,0,116,111,116,95,112,111,105,110,116,0,42,112,111,105,110,116,115,95,100,101,102,111,114,109,0,116,111,116,95,118,101,114,116,0,115,112,108,105,110,101,115,0,115,112,108,105,110,101,115,95,115,104,97,112,101,115,0,42,97,99,116,95,115,112,108,105,110,101,0,42,97,99,116,95,112,111,105,110,116,0,98,108,101,110,100,95,102,108,97,103,0,114,101,115,116,114,
105,99,116,102,108,97,103,0,42,105,109,97,103,101,95,117,115,101,114,0,42,117,118,110,97,109,101,0,42,97,116,116,114,105,98,117,116,101,95,110,97,109,101,0,118,97,108,105,100,0,105,110,116,101,114,112,0,42,115,105,109,97,0,115,116,114,111,107,101,95,114,103,98,97,91,52,93,0,102,105,108,108,95,114,103,98,97,91,52,93,0,109,105,120,95,114,103,98,97,91,52,93,0,115,116,114,111,107,101,95,115,116,121,108,101,0,102,105,108,108,
95,115,116,121,108,101,0,109,105,120,95,102,97,99,116,111,114,0,103,114,97,100,105,101,110,116,95,97,110,103,108,101,0,103,114,97,100,105,101,110,116,95,114,97,100,105,117,115,0,103,114,97,100,105,101,110,116,95,115,99,97,108,101,91,50,93,0,103,114,97,100,105,101,110,116,95,115,104,105,102,116,91,50,93,0,116,101,120,116,117,114,101,95,97,110,103,108,101,0,116,101,120,116,117,114,101,95,115,99,97,108,101,91,50,93,0,116,101,120,116,
117,114,101,95,111,102,102,115,101,116,91,50,93,0,116,101,120,116,117,114,101,95,111,112,97,99,105,116,121,0,116,101,120,116,117,114,101,95,112,105,120,115,105,122,101,0,103,114,97,100,105,101,110,116,95,116,121,112,101,0,109,105,120,95,115,116,114,111,107,101,95,102,97,99,116,111,114,0,97,108,105,103,110,109,101,110,116,95,109,111,100,101,0,109,97,116,95,111,99,99,108,117,115,105,111,110,0,105,110,116,101,114,115,101,99,116,105,111,110,95,
112,114,105,111,114,105,116,121,0,115,117,114,102,97,99,101,95,114,101,110,100,101,114,95,109,101,116,104,111,100,0,115,112,101,99,114,0,115,112,101,99,103,0,115,112,101,99,98,0,114,97,121,95,109,105,114,114,111,114,0,115,112,101,99,0,103,108,111,115,115,95,109,105,114,0,109,101,116,97,108,108,105,99,0,112,114,95,116,121,112,101,0,112,114,95,102,108,97,103,0,108,105,110,101,95,99,111,108,91,52,93,0,108,105,110,101,95,112,114,105,
111,114,105,116,121,0,118,99,111,108,95,97,108,112,104,97,0,112,97,105,110,116,95,97,99,116,105,118,101,95,115,108,111,116,0,112,97,105,110,116,95,99,108,111,110,101,95,115,108,111,116,0,116,111,116,95,115,108,111,116,115,0,100,105,115,112,108,97,99,101,109,101,110,116,95,109,101,116,104,111,100,0,116,104,105,99,107,110,101,115,115,95,109,111,100,101,0,97,108,112,104,97,95,116,104,114,101,115,104,111,108,100,0,114,101,102,114,97,99,116,
95,100,101,112,116,104,0,98,108,101,110,100,95,109,101,116,104,111,100,0,98,108,101,110,100,95,115,104,97,100,111,119,0,118,111,108,117,109,101,95,105,110,116,101,114,115,101,99,116,105,111,110,95,109,101,116,104,111,100,0,105,110,102,108,97,116,101,95,98,111,117,110,100,115,0,42,116,101,120,112,97,105,110,116,115,108,111,116,0,103,112,117,109,97,116,101,114,105,97,108,0,42,103,112,95,115,116,121,108,101,0,108,105,110,101,97,114,116,0,116,
111,116,101,100,103,101,0,116,111,116,112,111,108,121,0,116,111,116,108,111,111,112,0,42,112,111,108,121,95,111,102,102,115,101,116,95,105,110,100,105,99,101,115,0,118,100,97,116,97,0,101,100,97,116,97,0,112,100,97,116,97,0,108,100,97,116,97,0,42,109,115,101,108,101,99,116,0,116,111,116,115,101,108,101,99,116,0,97,99,116,95,102,97,99,101,0,42,116,101,120,99,111,109,101,115,104,0,101,100,105,116,102,108,97,103,0,115,109,111,111,
116,104,114,101,115,104,0,114,101,109,101,115,104,95,118,111,120,101,108,95,115,105,122,101,0,114,101,109,101,115,104,95,118,111,120,101,108,95,97,100,97,112,116,105,118,105,116,121,0,102,97,99,101,95,115,101,116,115,95,99,111,108,111,114,95,115,101,101,100,0,102,97,99,101,95,115,101,116,115,95,99,111,108,111,114,95,100,101,102,97,117,108,116,0,42,97,99,116,105,118,101,95,99,111,108,111,114,95,97,116,116,114,105,98,117,116,101,0,42,100,
101,102,97,117,108,116,95,99,111,108,111,114,95,97,116,116,114,105,98,117,116,101,0,42,97,99,116,105,118,101,95,117,118,95,109,97,112,95,97,116,116,114,105,98,117,116,101,0,42,100,101,102,97,117,108,116,95,117,118,95,109,97,112,95,97,116,116,114,105,98,117,116,101,0,114,101,109,101,115,104,95,109,111,100,101,0,99,100,95,102,108,97,103,0,115,117,98,100,105,118,114,0,115,117,98,115,117,114,102,116,121,112,101,0,42,109,112,111,108,121,
0,42,109,108,111,111,112,0,42,109,118,101,114,116,0,42,109,101,100,103,101,0,42,109,116,102,97,99,101,0,42,116,102,97,99,101,0,42,109,99,111,108,0,42,109,102,97,99,101,0,102,100,97,116,97,0,116,111,116,102,97,99,101,0,42,116,112,97,103,101,0,117,118,91,52,93,91,50,93,0,99,111,108,91,52,93,0,116,114,97,110,115,112,0,117,110,119,114,97,112,0,102,0,105,0,115,91,50,53,53,93,0,115,95,108,101,110,0,100,101,
102,95,110,114,0,42,100,119,0,116,111,116,119,101,105,103,104,116,0,114,97,100,105,117,115,91,51,93,0,116,111,116,100,105,115,112,0,40,42,100,105,115,112,115,41,40,41,0,42,104,105,100,100,101,110,0,118,49,0,118,50,0,99,114,101,97,115,101,0,98,119,101,105,103,104,116,0,108,111,111,112,115,116,97,114,116,0,117,118,91,50,93,0,101,0,118,51,0,118,52,0,101,100,99,111,100,101,0,42,98,98,0,101,120,112,120,0,101,120,112,
121,0,101,120,112,122,0,114,97,100,0,114,97,100,50,0,42,109,97,116,0,42,105,109,97,116,0,101,108,101,109,115,0,42,101,100,105,116,101,108,101,109,115,0,119,105,114,101,115,105,122,101,0,114,101,110,100,101,114,115,105,122,101,0,116,104,114,101,115,104,0,42,108,97,115,116,101,108,101,109,0,101,120,101,99,117,116,105,111,110,95,116,105,109,101,0,108,97,121,111,117,116,95,112,97,110,101,108,95,111,112,101,110,95,102,108,97,103,0,112,
101,114,115,105,115,116,101,110,116,95,117,105,100,0,42,116,101,120,116,117,114,101,0,42,109,97,112,95,111,98,106,101,99,116,0,109,97,112,95,98,111,110,101,91,54,52,93,0,117,118,108,97,121,101,114,95,116,109,112,0,116,101,120,109,97,112,112,105,110,103,0,115,117,98,100,105,118,84,121,112,101,0,108,101,118,101,108,115,0,114,101,110,100,101,114,76,101,118,101,108,115,0,117,118,95,115,109,111,111,116,104,0,113,117,97,108,105,116,121,0,
98,111,117,110,100,97,114,121,95,115,109,111,111,116,104,0,42,101,109,67,97,99,104,101,0,42,109,67,97,99,104,101,0,100,101,102,97,120,105,115,0,114,97,110,100,111,109,105,122,101,0,42,111,98,95,97,114,109,0,116,104,114,101,115,104,111,108,100,0,42,115,116,97,114,116,95,99,97,112,0,42,101,110,100,95,99,97,112,0,42,99,117,114,118,101,95,111,98,0,42,111,102,102,115,101,116,95,111,98,0,109,101,114,103,101,95,100,105,115,116,
0,102,105,116,95,116,121,112,101,0,111,102,102,115,101,116,95,116,121,112,101,0,117,118,95,111,102,102,115,101,116,91,50,93,0,98,105,115,101,99,116,95,116,104,114,101,115,104,111,108,100,0,117,115,101,95,99,111,114,114,101,99,116,95,111,114,100,101,114,95,111,110,95,109,101,114,103,101,0,117,118,95,111,102,102,115,101,116,95,99,111,112,121,91,50,93,0,42,109,105,114,114,111,114,95,111,98,0,115,112,108,105,116,95,97,110,103,108,101,0,
114,101,115,0,118,97,108,95,102,108,97,103,115,0,112,114,111,102,105,108,101,95,116,121,112,101,0,108,105,109,95,102,108,97,103,115,0,101,95,102,108,97,103,115,0,109,97,116,0,101,100,103,101,95,102,108,97,103,115,0,102,97,99,101,95,115,116,114,95,109,111,100,101,0,109,105,116,101,114,95,105,110,110,101,114,0,109,105,116,101,114,95,111,117,116,101,114,0,118,109,101,115,104,95,109,101,116,104,111,100,0,97,102,102,101,99,116,95,116,121,
112,101,0,112,114,111,102,105,108,101,0,98,101,118,101,108,95,97,110,103,108,101,0,100,101,102,103,114,112,95,110,97,109,101,91,54,52,93,0,42,99,117,115,116,111,109,95,112,114,111,102,105,108,101,0,101,100,103,101,95,119,101,105,103,104,116,95,110,97,109,101,91,54,52,93,0,118,101,114,116,101,120,95,119,101,105,103,104,116,95,110,97,109,101,91,54,52,93,0,42,100,111,109,97,105,110,0,42,102,108,111,119,0,42,101,102,102,101,99,116,
111,114,0,100,105,114,101,99,116,105,111,110,0,109,105,100,108,101,118,101,108,0,42,112,114,111,106,101,99,116,111,114,115,91,49,48,93,0,110,117,109,95,112,114,111,106,101,99,116,111,114,115,0,97,115,112,101,99,116,120,0,97,115,112,101,99,116,121,0,115,99,97,108,101,120,0,115,99,97,108,101,121,0,112,101,114,99,101,110,116,0,105,116,101,114,0,100,101,108,105,109,105,116,0,115,121,109,109,101,116,114,121,95,97,120,105,115,0,100,101,
102,103,114,112,95,102,97,99,116,111,114,0,102,97,99,101,95,99,111,117,110,116,0,102,97,99,0,42,111,98,106,101,99,116,99,101,110,116,101,114,0,42,95,112,97,100,52,0,42,105,110,100,101,120,97,114,0,116,111,116,105,110,100,101,120,0,42,99,108,111,116,104,79,98,106,101,99,116,0,42,115,105,109,95,112,97,114,109,115,0,42,99,111,108,108,95,112,97,114,109,115,0,42,112,111,105,110,116,95,99,97,99,104,101,0,42,104,97,105,114,
100,97,116,97,0,104,97,105,114,95,103,114,105,100,95,109,105,110,91,51,93,0,104,97,105,114,95,103,114,105,100,95,109,97,120,91,51,93,0,104,97,105,114,95,103,114,105,100,95,114,101,115,91,51,93,0,104,97,105,114,95,103,114,105,100,95,99,101,108,108,115,105,122,101,0,42,115,111,108,118,101,114,95,114,101,115,117,108,116,0,40,42,120,41,40,41,0,40,42,120,110,101,119,41,40,41,0,40,42,120,111,108,100,41,40,41,0,40,42,99,
117,114,114,101,110,116,95,120,110,101,119,41,40,41,0,40,42,99,117,114,114,101,110,116,95,120,41,40,41,0,40,42,99,117,114,114,101,110,116,95,118,41,40,41,0,40,42,118,101,114,116,95,116,114,105,115,41,40,41,0,109,118,101,114,116,95,110,117,109,0,116,114,105,95,110,117,109,0,116,105,109,101,95,120,0,116,105,109,101,95,120,110,101,119,0,105,115,95,115,116,97,116,105,99,0,42,98,118,104,116,114,101,101,0,40,42,118,101,114,116,
95,112,111,115,105,116,105,111,110,115,95,112,114,101,118,41,40,41,0,40,42,118,101,114,116,95,118,101,108,111,99,105,116,105,101,115,41,40,41,0,99,102,114,97,95,112,114,101,118,0,118,101,114,116,115,95,110,117,109,0,100,111,117,98,108,101,95,116,104,114,101,115,104,111,108,100,0,109,97,116,101,114,105,97,108,95,109,111,100,101,0,98,109,95,102,108,97,103,0,118,101,114,116,101,120,0,116,111,116,105,110,102,108,117,101,110,99,101,0,103,
114,105,100,115,105,122,101,0,42,98,105,110,100,105,110,102,108,117,101,110,99,101,115,0,42,98,105,110,100,111,102,102,115,101,116,115,0,42,98,105,110,100,99,97,103,101,99,111,115,0,116,111,116,99,97,103,101,118,101,114,116,0,42,100,121,110,103,114,105,100,0,42,100,121,110,105,110,102,108,117,101,110,99,101,115,0,42,100,121,110,118,101,114,116,115,0,100,121,110,103,114,105,100,115,105,122,101,0,100,121,110,99,101,108,108,109,105,110,91,51,
93,0,100,121,110,99,101,108,108,119,105,100,116,104,0,98,105,110,100,109,97,116,91,52,93,91,52,93,0,42,98,105,110,100,119,101,105,103,104,116,115,0,42,98,105,110,100,99,111,115,0,40,42,98,105,110,100,102,117,110,99,41,40,41,0,42,109,101,115,104,95,102,105,110,97,108,0,42,109,101,115,104,95,111,114,105,103,105,110,97,108,0,116,111,116,100,109,118,101,114,116,0,116,111,116,100,109,101,100,103,101,0,116,111,116,100,109,102,97,99,
101,0,112,115,121,115,0,112,111,115,105,116,105,111,110,0,114,97,110,100,111,109,95,112,111,115,105,116,105,111,110,0,114,97,110,100,111,109,95,114,111,116,97,116,105,111,110,0,112,97,114,116,105,99,108,101,95,97,109,111,117,110,116,0,112,97,114,116,105,99,108,101,95,111,102,102,115,101,116,0,105,110,100,101,120,95,108,97,121,101,114,95,110,97,109,101,91,54,56,93,0,118,97,108,117,101,95,108,97,121,101,114,95,110,97,109,101,91,54,56,
93,0,42,102,97,99,101,112,97,0,118,103,114,111,117,112,0,112,114,111,116,101,99,116,0,117,118,110,97,109,101,91,54,56,93,0,108,118,108,0,115,99,117,108,112,116,108,118,108,0,114,101,110,100,101,114,108,118,108,0,116,111,116,108,118,108,0,115,105,109,112,108,101,0,42,102,115,115,0,42,97,117,120,84,97,114,103,101,116,0,118,103,114,111,117,112,95,110,97,109,101,91,54,52,93,0,107,101,101,112,68,105,115,116,0,115,104,114,105,110,
107,79,112,116,115,0,115,117,98,115,117,114,102,76,101,118,101,108,115,0,42,111,114,105,103,105,110,0,108,105,109,105,116,91,50,93,0,100,101,102,111,114,109,95,97,120,105,115,0,115,104,101,108,108,95,100,101,102,103,114,112,95,110,97,109,101,91,54,52,93,0,114,105,109,95,100,101,102,103,114,112,95,110,97,109,101,91,54,52,93,0,111,102,102,115,101,116,95,102,97,99,95,118,103,0,111,102,102,115,101,116,95,99,108,97,109,112,0,110,111,
110,109,97,110,105,102,111,108,100,95,111,102,102,115,101,116,95,109,111,100,101,0,110,111,110,109,97,110,105,102,111,108,100,95,98,111,117,110,100,97,114,121,95,109,111,100,101,0,99,114,101,97,115,101,95,105,110,110,101,114,0,99,114,101,97,115,101,95,111,117,116,101,114,0,99,114,101,97,115,101,95,114,105,109,0,109,97,116,95,111,102,115,0,109,97,116,95,111,102,115,95,114,105,109,0,109,101,114,103,101,95,116,111,108,101,114,97,110,99,101,
0,98,101,118,101,108,95,99,111,110,118,101,120,0,42,111,98,95,97,120,105,115,0,115,116,101,112,115,0,114,101,110,100,101,114,95,115,116,101,112,115,0,115,99,114,101,119,95,111,102,115,0,42,111,99,101,97,110,0,42,111,99,101,97,110,99,97,99,104,101,0,114,101,115,111,108,117,116,105,111,110,0,118,105,101,119,112,111,114,116,95,114,101,115,111,108,117,116,105,111,110,0,115,112,97,116,105,97,108,95,115,105,122,101,0,119,105,110,100,95,
118,101,108,111,99,105,116,121,0,115,109,97,108,108,101,115,116,95,119,97,118,101,0,119,97,118,101,95,97,108,105,103,110,109,101,110,116,0,119,97,118,101,95,100,105,114,101,99,116,105,111,110,0,119,97,118,101,95,115,99,97,108,101,0,99,104,111,112,95,97,109,111,117,110,116,0,102,111,97,109,95,99,111,118,101,114,97,103,101,0,115,112,101,99,116,114,117,109,0,102,101,116,99,104,95,106,111,110,115,119,97,112,0,115,104,97,114,112,101,110,
95,112,101,97,107,95,106,111,110,115,119,97,112,0,98,97,107,101,115,116,97,114,116,0,98,97,107,101,101,110,100,0,99,97,99,104,101,112,97,116,104,91,49,48,50,52,93,0,102,111,97,109,108,97,121,101,114,110,97,109,101,91,54,56,93,0,115,112,114,97,121,108,97,121,101,114,110,97,109,101,91,54,56,93,0,99,97,99,104,101,100,0,103,101,111,109,101,116,114,121,95,109,111,100,101,0,114,101,112,101,97,116,95,120,0,114,101,112,101,97,
116,95,121,0,102,111,97,109,95,102,97,100,101,0,42,111,98,106,101,99,116,95,102,114,111,109,0,42,111,98,106,101,99,116,95,116,111,0,98,111,110,101,95,102,114,111,109,91,54,52,93,0,98,111,110,101,95,116,111,91,54,52,93,0,102,97,108,108,111,102,102,95,114,97,100,105,117,115,0,101,100,105,116,95,102,108,97,103,115,0,100,101,102,97,117,108,116,95,119,101,105,103,104,116,0,42,99,109,97,112,95,99,117,114,118,101,0,97,100,100,
95,116,104,114,101,115,104,111,108,100,0,114,101,109,95,116,104,114,101,115,104,111,108,100,0,109,97,115,107,95,99,111,110,115,116,97,110,116,0,109,97,115,107,95,100,101,102,103,114,112,95,110,97,109,101,91,54,52,93,0,109,97,115,107,95,116,101,120,95,117,115,101,95,99,104,97,110,110,101,108,0,42,109,97,115,107,95,116,101,120,116,117,114,101,0,42,109,97,115,107,95,116,101,120,95,109,97,112,95,111,98,106,0,109,97,115,107,95,116,101,
120,95,109,97,112,95,98,111,110,101,91,54,52,93,0,109,97,115,107,95,116,101,120,95,109,97,112,112,105,110,103,0,109,97,115,107,95,116,101,120,95,117,118,108,97,121,101,114,95,110,97,109,101,91,54,56,93,0,100,101,102,103,114,112,95,110,97,109,101,95,97,91,54,52,93,0,100,101,102,103,114,112,95,110,97,109,101,95,98,91,54,52,93,0,100,101,102,97,117,108,116,95,119,101,105,103,104,116,95,97,0,100,101,102,97,117,108,116,95,119,
101,105,103,104,116,95,98,0,109,105,120,95,115,101,116,0,112,114,111,120,105,109,105,116,121,95,109,111,100,101,0,112,114,111,120,105,109,105,116,121,95,102,108,97,103,115,0,42,112,114,111,120,105,109,105,116,121,95,111,98,95,116,97,114,103,101,116,0,109,105,110,95,100,105,115,116,0,109,97,120,95,100,105,115,116,0,42,98,114,117,115,104,0,104,101,114,109,105,116,101,95,110,117,109,0,118,111,120,101,108,95,115,105,122,101,0,97,100,97,112,
116,105,118,105,116,121,0,98,114,97,110,99,104,95,115,109,111,111,116,104,105,110,103,0,115,121,109,109,101,116,114,121,95,97,120,101,115,0,113,117,97,100,95,109,101,116,104,111,100,0,110,103,111,110,95,109,101,116,104,111,100,0,109,105,110,95,118,101,114,116,105,99,101,115,0,108,97,109,98,100,97,0,108,97,109,98,100,97,95,98,111,114,100,101,114,0,40,42,100,101,108,116,97,115,41,40,41,0,100,101,108,116,97,115,95,110,117,109,0,115,
109,111,111,116,104,95,116,121,112,101,0,114,101,115,116,95,115,111,117,114,99,101,0,40,42,98,105,110,100,95,99,111,111,114,100,115,41,40,41,0,98,105,110,100,95,99,111,111,114,100,115,95,110,117,109,0,100,101,108,116,97,95,99,97,99,104,101,0,97,120,105,115,95,117,0,97,120,105,115,95,118,0,99,101,110,116,101,114,91,50,93,0,42,111,98,106,101,99,116,95,115,114,99,0,98,111,110,101,95,115,114,99,91,54,52,93,0,42,111,98,
106,101,99,116,95,100,115,116,0,98,111,110,101,95,100,115,116,91,54,52,93,0,112,108,97,121,95,109,111,100,101,0,102,108,105,112,95,97,120,105,115,0,100,101,102,111,114,109,95,109,111,100,101,0,101,118,97,108,95,102,114,97,109,101,0,101,118,97,108,95,102,97,99,116,111,114,0,97,110,99,104,111,114,95,103,114,112,95,110,97,109,101,91,54,52,93,0,116,111,116,97,108,95,118,101,114,116,115,0,42,118,101,114,116,101,120,99,111,0,42,
99,97,99,104,101,95,115,121,115,116,101,109,0,99,114,101,97,115,101,95,119,101,105,103,104,116,0,42,111,98,95,115,111,117,114,99,101,0,100,97,116,97,95,116,121,112,101,115,0,118,109,97,112,95,109,111,100,101,0,101,109,97,112,95,109,111,100,101,0,108,109,97,112,95,109,111,100,101,0,112,109,97,112,95,109,111,100,101,0,109,97,112,95,109,97,120,95,100,105,115,116,97,110,99,101,0,109,97,112,95,114,97,121,95,114,97,100,105,117,115,
0,105,115,108,97,110,100,115,95,112,114,101,99,105,115,105,111,110,0,108,97,121,101,114,115,95,115,101,108,101,99,116,95,115,114,99,91,53,93,0,108,97,121,101,114,115,95,115,101,108,101,99,116,95,100,115,116,91,53,93,0,109,105,120,95,108,105,109,105,116,0,114,101,97,100,95,102,108,97,103,0,42,118,101,114,116,95,105,110,100,115,0,42,118,101,114,116,95,119,101,105,103,104,116,115,0,110,111,114,109,97,108,95,100,105,115,116,0,42,98,
105,110,100,115,0,110,117,109,98,105,110,100,115,0,118,101,114,116,101,120,95,105,100,120,0,42,118,101,114,116,115,0,110,117,109,95,109,101,115,104,95,118,101,114,116,115,0,116,97,114,103,101,116,95,118,101,114,116,115,95,110,117,109,0,110,117,109,112,111,108,121,0,42,105,100,95,110,97,109,101,0,42,108,105,98,95,110,97,109,101,0,42,112,97,99,107,101,100,95,102,105,108,101,0,109,101,116,97,95,102,105,108,101,115,95,110,117,109,0,98,
108,111,98,95,102,105,108,101,115,95,110,117,109,0,42,109,101,116,97,95,102,105,108,101,115,0,42,98,108,111,98,95,102,105,108,101,115,0,98,97,107,101,95,109,111,100,101,0,98,97,107,101,95,116,97,114,103,101,116,0,42,100,105,114,101,99,116,111,114,121,0,100,97,116,97,95,98,108,111,99,107,115,95,110,117,109,0,97,99,116,105,118,101,95,100,97,116,97,95,98,108,111,99,107,0,42,100,97,116,97,95,98,108,111,99,107,115,0,42,112,
97,99,107,101,100,0,98,97,107,101,95,115,105,122,101,0,42,110,111,100,101,95,103,114,111,117,112,0,115,101,116,116,105,110,103,115,0,42,115,105,109,117,108,97,116,105,111,110,95,98,97,107,101,95,100,105,114,101,99,116,111,114,121,0,98,97,107,101,115,95,110,117,109,0,42,98,97,107,101,115,0,112,97,110,101,108,115,95,110,117,109,0,42,112,97,110,101,108,115,0,114,101,115,111,108,117,116,105,111,110,95,109,111,100,101,0,118,111,120,101,
108,95,97,109,111,117,110,116,0,105,110,116,101,114,105,111,114,95,98,97,110,100,95,119,105,100,116,104,0,42,116,101,120,116,117,114,101,95,109,97,112,95,111,98,106,101,99,116,0,116,101,120,116,117,114,101,95,109,97,112,95,109,111,100,101,0,116,101,120,116,117,114,101,95,109,105,100,95,108,101,118,101,108,91,51,93,0,116,101,120,116,117,114,101,95,115,97,109,112,108,101,95,114,97,100,105,117,115,0,103,114,105,100,95,110,97,109,101,91,54,
52,93,0,108,97,121,101,114,95,110,97,109,101,91,54,52,93,0,109,97,116,101,114,105,97,108,95,112,97,115,115,0,118,101,114,116,101,120,95,103,114,111,117,112,95,110,97,109,101,91,54,52,93,0,42,99,117,115,116,111,109,95,99,117,114,118,101,0,99,111,108,111,114,95,102,97,99,116,111,114,0,104,97,114,100,110,101,115,115,95,102,97,99,116,111,114,0,95,112,97,100,91,56,93,0,116,105,110,116,95,109,111,100,101,0,115,116,114,111,107,
101,95,108,111,99,91,51,93,0,115,116,114,111,107,101,95,114,111,116,91,51,93,0,115,116,114,111,107,101,95,115,99,97,108,101,91,51,93,0,42,115,101,103,109,101,110,116,115,95,97,114,114,97,121,0,115,101,103,109,101,110,116,115,95,110,117,109,0,109,97,115,107,95,115,119,105,116,99,104,101,115,0,109,97,116,101,114,105,97,108,95,109,97,115,107,95,98,105,116,115,0,42,115,104,97,114,101,100,95,99,97,99,104,101,0,115,101,103,109,101,
110,116,95,115,116,97,114,116,0,115,101,103,109,101,110,116,95,101,110,100,0,115,101,103,109,101,110,116,95,109,111,100,101,0,115,101,103,109,101,110,116,95,114,101,112,101,97,116,0,114,101,110,100,101,114,95,115,105,122,101,0,114,101,110,100,101,114,95,102,108,97,103,0,100,105,114,91,55,54,56,93,0,116,99,0,98,117,105,108,100,95,115,105,122,101,95,102,108,97,103,0,98,117,105,108,100,95,116,99,95,102,108,97,103,0,117,115,101,114,0,
42,103,112,117,116,101,120,116,117,114,101,91,51,93,0,103,112,117,116,101,120,116,117,114,101,115,0,108,97,115,116,115,105,122,101,91,50,93,0,42,103,112,100,0,116,114,97,99,107,105,110,103,0,42,116,114,97,99,107,105,110,103,95,99,111,110,116,101,120,116,0,112,114,111,120,121,0,117,115,101,95,116,114,97,99,107,95,109,97,115,107,0,116,114,97,99,107,95,112,114,101,118,105,101,119,95,104,101,105,103,104,116,0,102,114,97,109,101,95,119,
105,100,116,104,0,102,114,97,109,101,95,104,101,105,103,104,116,0,117,110,100,105,115,116,95,109,97,114,107,101,114,0,42,116,114,97,99,107,95,115,101,97,114,99,104,0,42,116,114,97,99,107,95,112,114,101,118,105,101,119,0,116,114,97,99,107,95,112,111,115,91,50,93,0,116,114,97,99,107,95,100,105,115,97,98,108,101,100,0,116,114,97,99,107,95,108,111,99,107,101,100,0,115,99,101,110,101,95,102,114,97,109,101,110,114,0,42,116,114,97,
99,107,0,42,109,97,114,107,101,114,0,115,108,105,100,101,95,115,99,97,108,101,91,50,93,0,99,104,97,110,110,101,108,91,51,50,93,0,110,111,105,115,101,115,105,122,101,0,116,117,114,98,117,108,0,110,111,95,114,111,116,95,97,120,105,115,0,115,116,114,105,100,101,95,97,120,105,115,0,99,117,114,109,111,100,0,97,99,116,111,102,102,115,0,115,116,114,105,100,101,108,101,110,0,115,116,114,105,100,101,99,104,97,110,110,101,108,91,51,50,
93,0,111,102,102,115,95,98,111,110,101,91,51,50,93,0,105,116,101,109,95,116,121,112,101,0,105,116,101,109,0,42,115,111,99,107,101,116,95,116,121,112,101,0,97,116,116,114,105,98,117,116,101,95,100,111,109,97,105,110,0,100,101,102,97,117,108,116,95,105,110,112,117,116,0,42,100,101,102,97,117,108,116,95,97,116,116,114,105,98,117,116,101,95,110,97,109,101,0,42,115,111,99,107,101,116,95,100,97,116,97,0,115,116,114,117,99,116,117,114,
101,95,116,121,112,101,0,42,42,105,116,101,109,115,95,97,114,114,97,121,0,105,116,101,109,115,95,110,117,109,0,105,100,101,110,116,105,102,105,101,114,0,114,111,111,116,95,112,97,110,101,108,0,97,99,116,105,118,101,95,105,110,100,101,120,0,110,101,120,116,95,117,105,100,0,104,97,115,105,110,112,117,116,0,104,97,115,111,117,116,112,117,116,0,100,97,116,97,116,121,112,101,0,115,111,99,107,101,116,116,121,112,101,0,105,115,95,99,111,112,
121,0,101,120,116,101,114,110,97,108,0,105,100,101,110,116,105,102,105,101,114,91,54,52,93,0,42,115,116,111,114,97,103,101,0,108,105,109,105,116,0,105,110,95,111,117,116,0,42,116,121,112,101,105,110,102,111,0,115,116,97,99,107,95,105,110,100,101,120,0,100,105,115,112,108,97,121,95,115,104,97,112,101,0,115,104,111,114,116,95,108,97,98,101,108,91,54,52,93,0,100,101,115,99,114,105,112,116,105,111,110,91,54,52,93,0,111,119,110,95,
105,110,100,101,120,0,116,111,95,105,110,100,101,120,0,42,108,105,110,107,0,110,115,0,105,110,112,117,116,115,0,111,117,116,112,117,116,115,0,117,105,95,111,114,100,101,114,0,99,117,115,116,111,109,49,0,99,117,115,116,111,109,50,0,99,117,115,116,111,109,51,0,99,117,115,116,111,109,52,0,119,97,114,110,105,110,103,95,112,114,111,112,97,103,97,116,105,111,110,0,108,111,99,97,116,105,111,110,91,50,93,0,108,111,99,120,0,108,111,99,
121,0,111,102,102,115,101,116,120,0,111,102,102,115,101,116,121,0,110,117,109,95,112,97,110,101,108,95,115,116,97,116,101,115,0,42,112,97,110,101,108,95,115,116,97,116,101,115,95,97,114,114,97,121,0,42,102,114,111,109,110,111,100,101,0,42,116,111,110,111,100,101,0,42,102,114,111,109,115,111,99,107,0,42,116,111,115,111,99,107,0,109,117,108,116,105,95,105,110,112,117,116,95,115,111,99,107,101,116,95,105,110,100,101,120,0,110,111,100,101,
95,105,100,0,105,100,95,105,110,95,110,111,100,101,0,112,97,116,104,0,118,105,101,119,95,99,101,110,116,101,114,91,50,93,0,110,111,100,101,115,0,108,105,110,107,115,0,99,117,114,95,105,110,100,101,120,0,99,104,117,110,107,115,105,122,101,0,101,120,101,99,117,116,105,111,110,95,109,111,100,101,0,100,101,102,97,117,108,116,95,103,114,111,117,112,95,110,111,100,101,95,119,105,100,116,104,0,118,105,101,119,101,114,95,98,111,114,100,101,114,
0,116,114,101,101,95,105,110,116,101,114,102,97,99,101,0,97,99,116,105,118,101,95,118,105,101,119,101,114,95,107,101,121,0,110,101,115,116,101,100,95,110,111,100,101,95,114,101,102,115,95,110,117,109,0,42,110,101,115,116,101,100,95,110,111,100,101,95,114,101,102,115,0,42,103,101,111,109,101,116,114,121,95,110,111,100,101,95,97,115,115,101,116,95,116,114,97,105,116,115,0,118,97,108,117,101,91,52,93,0,100,105,109,101,110,115,105,111,110,115,
0,118,97,108,117,101,95,101,117,108,101,114,91,51,93,0,118,97,108,117,101,91,49,48,50,52,93,0,42,118,97,108,117,101,0,108,97,98,101,108,95,115,105,122,101,0,116,121,112,101,95,105,100,110,97,109,101,91,54,52,93,0,110,114,0,99,121,99,108,105,99,0,109,111,118,105,101,0,115,97,116,117,114,97,116,105,111,110,0,99,111,110,116,114,97,115,116,0,103,97,105,110,0,108,105,102,116,0,109,97,115,116,101,114,0,115,104,97,100,111,
119,115,0,109,105,100,116,111,110,101,115,0,104,105,103,104,108,105,103,104,116,115,0,115,116,97,114,116,109,105,100,116,111,110,101,115,0,101,110,100,109,105,100,116,111,110,101,115,0,102,108,97,112,115,0,114,111,117,110,100,105,110,103,0,99,97,116,97,100,105,111,112,116,114,105,99,0,108,101,110,115,115,104,105,102,116,0,112,97,115,115,95,110,97,109,101,91,54,52,93,0,115,105,122,101,120,0,115,105,122,101,121,0,109,97,120,115,112,101,101,
100,0,109,105,110,115,112,101,101,100,0,97,115,112,101,99,116,0,99,117,114,118,101,100,0,112,101,114,99,101,110,116,120,0,112,101,114,99,101,110,116,121,0,102,105,108,116,101,114,116,121,112,101,0,98,111,107,101,104,0,105,109,97,103,101,95,105,110,95,119,105,100,116,104,0,105,109,97,103,101,95,105,110,95,104,101,105,103,104,116,0,99,101,110,116,101,114,95,120,0,99,101,110,116,101,114,95,121,0,115,112,105,110,0,122,111,111,109,0,115,
105,103,109,97,95,99,111,108,111,114,0,115,105,103,109,97,95,115,112,97,99,101,0,118,97,114,105,97,116,105,111,110,0,117,110,105,102,111,114,109,105,116,121,0,115,104,97,114,112,110,101,115,115,0,101,99,99,101,110,116,114,105,99,105,116,121,0,104,105,103,104,95,112,114,101,99,105,115,105,111,110,0,99,111,110,116,114,97,115,116,95,108,105,109,105,116,0,99,111,114,110,101,114,95,114,111,117,110,100,105,110,103,0,104,117,101,0,115,97,116,
0,105,109,95,102,111,114,109,97,116,0,98,97,115,101,95,112,97,116,104,91,49,48,50,52,93,0,97,99,116,105,118,101,95,105,110,112,117,116,0,115,97,118,101,95,97,115,95,114,101,110,100,101,114,0,117,115,101,95,114,101,110,100,101,114,95,102,111,114,109,97,116,0,117,115,101,95,110,111,100,101,95,102,111,114,109,97,116,0,112,97,116,104,91,49,48,50,52,93,0,108,97,121,101,114,91,54,50,93,0,116,49,0,116,50,0,116,51,0,102,
115,116,114,101,110,103,116,104,0,102,97,108,112,104,97,0,107,101,121,91,52,93,0,97,108,103,111,114,105,116,104,109,0,99,104,97,110,110,101,108,0,120,49,0,120,50,0,121,49,0,121,50,0,102,97,99,95,120,49,0,102,97,99,95,120,50,0,102,97,99,95,121,49,0,102,97,99,95,121,50,0,121,99,99,95,109,111,100,101,0,98,107,116,121,112,101,0,112,114,101,118,105,101,119,0,103,97,109,99,111,0,110,111,95,122,98,117,102,0,109,
97,120,98,108,117,114,0,98,116,104,114,101,115,104,0,42,100,105,99,116,0,42,110,111,100,101,0,115,116,97,114,95,52,53,0,115,116,114,101,97,107,115,0,99,111,108,109,111,100,0,109,105,120,0,102,97,100,101,0,97,110,103,108,101,95,111,102,115,0,107,101,121,0,109,0,99,0,106,105,116,0,112,114,111,106,0,102,105,116,0,100,105,115,116,111,114,116,105,111,110,95,116,121,112,101,0,115,108,111,112,101,91,51,93,0,112,111,119,101,114,
91,51,93,0,111,102,102,115,101,116,95,98,97,115,105,115,0,108,105,102,116,91,51,93,0,103,97,109,109,97,91,51,93,0,103,97,105,110,91,51,93,0,105,110,112,117,116,95,116,101,109,112,101,114,97,116,117,114,101,0,105,110,112,117,116,95,116,105,110,116,0,111,117,116,112,117,116,95,116,101,109,112,101,114,97,116,117,114,101,0,111,117,116,112,117,116,95,116,105,110,116,0,108,105,109,99,104,97,110,0,117,110,115,112,105,108,108,0,108,105,
109,115,99,97,108,101,0,117,115,112,105,108,108,114,0,117,115,112,105,108,108,103,0,117,115,112,105,108,108,98,0,102,114,111,109,95,99,111,108,111,114,95,115,112,97,99,101,91,54,52,93,0,116,111,95,99,111,108,111,114,95,115,112,97,99,101,91,54,52,93,0,115,105,122,101,95,120,0,115,105,122,101,95,121,0,116,101,120,95,109,97,112,112,105,110,103,0,99,111,108,111,114,95,109,97,112,112,105,110,103,0,115,107,121,95,109,111,100,101,108,
0,115,117,110,95,100,105,114,101,99,116,105,111,110,91,51,93,0,116,117,114,98,105,100,105,116,121,0,103,114,111,117,110,100,95,97,108,98,101,100,111,0,115,117,110,95,115,105,122,101,0,115,117,110,95,105,110,116,101,110,115,105,116,121,0,115,117,110,95,101,108,101,118,97,116,105,111,110,0,115,117,110,95,114,111,116,97,116,105,111,110,0,97,108,116,105,116,117,100,101,0,97,105,114,95,100,101,110,115,105,116,121,0,100,117,115,116,95,100,101,
110,115,105,116,121,0,111,122,111,110,101,95,100,101,110,115,105,116,121,0,115,117,110,95,100,105,115,99,0,99,111,108,111,114,95,115,112,97,99,101,0,112,114,111,106,101,99,116,105,111,110,0,112,114,111,106,101,99,116,105,111,110,95,98,108,101,110,100,0,105,110,116,101,114,112,111,108,97,116,105,111,110,0,101,120,116,101,110,115,105,111,110,0,111,102,102,115,101,116,95,102,114,101,113,0,115,113,117,97,115,104,95,102,114,101,113,0,115,113,117,
97,115,104,0,110,111,114,109,97,108,105,122,101,0,102,101,97,116,117,114,101,0,99,111,108,111,114,105,110,103,0,109,117,115,103,114,97,118,101,95,116,121,112,101,0,98,97,110,100,115,95,100,105,114,101,99,116,105,111,110,0,114,105,110,103,115,95,100,105,114,101,99,116,105,111,110,0,119,97,118,101,95,112,114,111,102,105,108,101,0,110,97,109,101,91,50,53,54,93,0,99,111,110,118,101,114,116,95,102,114,111,109,0,99,111,110,118,101,114,116,
95,116,111,0,112,111,105,110,116,95,115,111,117,114,99,101,0,112,97,114,116,105,99,108,101,95,115,121,115,116,101,109,0,99,111,108,111,114,95,115,111,117,114,99,101,0,111,98,95,99,111,108,111,114,95,115,111,117,114,99,101,0,112,100,0,99,97,99,104,101,100,95,114,101,115,111,108,117,116,105,111,110,0,118,101,114,116,101,120,95,97,116,116,114,105,98,117,116,101,95,110,97,109,101,91,54,56,93,0,117,115,101,95,115,117,98,115,117,114,102,
97,99,101,95,97,117,116,111,95,114,97,100,105,117,115,0,109,111,100,101,108,0,112,97,114,97,109,101,116,114,105,122,97,116,105,111,110,0,116,114,97,99,107,105,110,103,95,111,98,106,101,99,116,91,54,52,93,0,115,109,111,111,116,104,110,101,115,115,0,115,99,114,101,101,110,95,98,97,108,97,110,99,101,0,100,101,115,112,105,108,108,95,102,97,99,116,111,114,0,100,101,115,112,105,108,108,95,98,97,108,97,110,99,101,0,101,100,103,101,95,
107,101,114,110,101,108,95,114,97,100,105,117,115,0,101,100,103,101,95,107,101,114,110,101,108,95,116,111,108,101,114,97,110,99,101,0,99,108,105,112,95,98,108,97,99,107,0,99,108,105,112,95,119,104,105,116,101,0,100,105,108,97,116,101,95,100,105,115,116,97,110,99,101,0,102,101,97,116,104,101,114,95,100,105,115,116,97,110,99,101,0,102,101,97,116,104,101,114,95,102,97,108,108,111,102,102,0,98,108,117,114,95,112,114,101,0,98,108,117,114,
95,112,111,115,116,0,116,114,97,99,107,95,110,97,109,101,91,54,52,93,0,119,114,97,112,95,97,120,105,115,0,112,108,97,110,101,95,116,114,97,99,107,95,110,97,109,101,91,54,52,93,0,109,111,116,105,111,110,95,98,108,117,114,95,115,97,109,112,108,101,115,0,109,111,116,105,111,110,95,98,108,117,114,95,115,104,117,116,116,101,114,0,98,121,116,101,99,111,100,101,95,104,97,115,104,91,54,52,93,0,42,98,121,116,101,99,111,100,101,0,
100,105,114,101,99,116,105,111,110,95,116,121,112,101,0,117,118,95,109,97,112,91,54,52,93,0,115,111,117,114,99,101,91,50,93,0,114,97,121,95,108,101,110,103,116,104,0,101,110,99,111,100,101,100,95,104,97,115,104,0,97,100,100,91,51,93,0,114,101,109,111,118,101,91,51,93,0,101,110,116,114,105,101,115,0,42,109,97,116,116,101,95,105,100,0,110,117,109,95,105,110,112,117,116,115,0,104,100,114,0,112,114,101,102,105,108,116,101,114,0,
105,110,116,101,114,112,111,108,97,116,105,111,110,95,116,121,112,101,0,98,111,111,108,101,97,110,0,105,110,116,101,103,101,114,0,114,111,116,97,116,105,111,110,95,101,117,108,101,114,91,51,93,0,118,101,99,116,111,114,91,51,93,0,42,115,116,114,105,110,103,0,116,114,97,110,115,102,111,114,109,95,115,112,97,99,101,0,105,110,112,117,116,95,116,121,112,101,95,114,97,100,105,117,115,0,116,97,114,103,101,116,95,101,108,101,109,101,110,116,0,
102,105,108,108,95,116,121,112,101,0,99,111,117,110,116,95,109,111,100,101,0,105,110,112,117,116,95,116,121,112,101,0,42,105,116,101,109,115,95,97,114,114,97,121,0,110,101,120,116,95,105,100,101,110,116,105,102,105,101,114,0,101,110,117,109,95,100,101,102,105,110,105,116,105,111,110,0,115,112,108,105,110,101,95,116,121,112,101,0,104,97,110,100,108,101,95,116,121,112,101,0,107,101,101,112,95,108,97,115,116,95,115,101,103,109,101,110,116,0,117,
115,101,95,97,108,108,95,99,117,114,118,101,115,0,109,97,112,112,105,110,103,0,42,99,97,112,116,117,114,101,95,105,116,101,109,115,0,99,97,112,116,117,114,101,95,105,116,101,109,115,95,110,117,109,0,97,108,105,103,110,95,120,0,112,105,118,111,116,95,109,111,100,101,0,109,101,116,104,111,100,0,115,111,99,107,101,116,95,116,121,112,101,0,111,117,116,112,117,116,95,110,111,100,101,95,105,100,0,42,105,116,101,109,115,0,105,110,115,112,101,
99,116,105,111,110,95,105,110,100,101,120,0,105,110,112,117,116,95,105,116,101,109,115,0,109,97,105,110,95,105,116,101,109,115,0,103,101,110,101,114,97,116,105,111,110,95,105,116,101,109,115,0,111,117,116,112,117,116,95,105,116,101,109,115,0,102,97,99,116,111,114,95,109,111,100,101,0,99,108,97,109,112,95,102,97,99,116,111,114,0,99,108,97,109,112,95,114,101,115,117,108,116,0,98,108,101,110,100,95,116,121,112,101,0,99,111,108,111,114,95,
105,100,0,100,114,97,119,95,115,116,121,108,101,0,118,101,108,91,51,93,0,116,104,114,101,97,100,115,0,115,104,111,119,95,97,100,118,97,110,99,101,100,111,112,116,105,111,110,115,0,114,101,115,111,108,117,116,105,111,110,120,121,122,0,112,114,101,118,105,101,119,114,101,115,120,121,122,0,114,101,97,108,115,105,122,101,0,103,117,105,68,105,115,112,108,97,121,77,111,100,101,0,114,101,110,100,101,114,68,105,115,112,108,97,121,77,111,100,101,0,
118,105,115,99,111,115,105,116,121,86,97,108,117,101,0,118,105,115,99,111,115,105,116,121,77,111,100,101,0,118,105,115,99,111,115,105,116,121,69,120,112,111,110,101,110,116,0,103,114,97,118,91,51,93,0,97,110,105,109,83,116,97,114,116,0,97,110,105,109,69,110,100,0,98,97,107,101,83,116,97,114,116,0,98,97,107,101,69,110,100,0,102,114,97,109,101,79,102,102,115,101,116,0,103,115,116,97,114,0,109,97,120,82,101,102,105,110,101,0,105,
110,105,86,101,108,120,0,105,110,105,86,101,108,121,0,105,110,105,86,101,108,122,0,115,117,114,102,100,97,116,97,80,97,116,104,91,49,48,50,52,93,0,98,98,83,116,97,114,116,91,51,93,0,98,98,83,105,122,101,91,51,93,0,116,121,112,101,70,108,97,103,115,0,100,111,109,97,105,110,78,111,118,101,99,103,101,110,0,118,111,108,117,109,101,73,110,105,116,84,121,112,101,0,112,97,114,116,83,108,105,112,86,97,108,117,101,0,103,101,110,
101,114,97,116,101,84,114,97,99,101,114,115,0,103,101,110,101,114,97,116,101,80,97,114,116,105,99,108,101,115,0,115,117,114,102,97,99,101,83,109,111,111,116,104,105,110,103,0,115,117,114,102,97,99,101,83,117,98,100,105,118,115,0,112,97,114,116,105,99,108,101,73,110,102,83,105,122,101,0,112,97,114,116,105,99,108,101,73,110,102,65,108,112,104,97,0,102,97,114,70,105,101,108,100,83,105,122,101,0,42,109,101,115,104,86,101,108,111,99,105,
116,105,101,115,0,99,112,115,84,105,109,101,83,116,97,114,116,0,99,112,115,84,105,109,101,69,110,100,0,99,112,115,81,117,97,108,105,116,121,0,97,116,116,114,97,99,116,102,111,114,99,101,83,116,114,101,110,103,116,104,0,97,116,116,114,97,99,116,102,111,114,99,101,82,97,100,105,117,115,0,118,101,108,111,99,105,116,121,102,111,114,99,101,83,116,114,101,110,103,116,104,0,118,101,108,111,99,105,116,121,102,111,114,99,101,82,97,100,105,117,
115,0,108,97,115,116,103,111,111,100,102,114,97,109,101,0,97,110,105,109,82,97,116,101,0,100,101,102,108,101,99,116,0,102,111,114,99,101,102,105,101,108,100,0,115,104,97,112,101,0,116,101,120,95,109,111,100,101,0,107,105,110,107,0,107,105,110,107,95,97,120,105,115,0,122,100,105,114,0,102,95,115,116,114,101,110,103,116,104,0,102,95,100,97,109,112,0,102,95,102,108,111,119,0,102,95,119,105,110,100,95,102,97,99,116,111,114,0,102,95,
115,105,122,101,0,102,95,112,111,119,101,114,0,109,97,120,100,105,115,116,0,109,105,110,100,105,115,116,0,102,95,112,111,119,101,114,95,114,0,109,97,120,114,97,100,0,109,105,110,114,97,100,0,112,100,101,102,95,100,97,109,112,0,112,100,101,102,95,114,100,97,109,112,0,112,100,101,102,95,112,101,114,109,0,112,100,101,102,95,102,114,105,99,116,0,112,100,101,102,95,114,102,114,105,99,116,0,112,100,101,102,95,115,116,105,99,107,110,101,115,
115,0,97,98,115,111,114,112,116,105,111,110,0,112,100,101,102,95,115,98,100,97,109,112,0,112,100,101,102,95,115,98,105,102,116,0,112,100,101,102,95,115,98,111,102,116,0,99,108,117,109,112,95,102,97,99,0,99,108,117,109,112,95,112,111,119,0,107,105,110,107,95,102,114,101,113,0,107,105,110,107,95,115,104,97,112,101,0,107,105,110,107,95,97,109,112,0,102,114,101,101,95,101,110,100,0,116,101,120,95,110,97,98,108,97,0,102,95,110,111,
105,115,101,0,100,114,97,119,118,101,99,49,91,52,93,0,100,114,97,119,118,101,99,50,91,52,93,0,100,114,97,119,118,101,99,95,102,97,108,108,111,102,102,95,109,105,110,91,51,93,0,100,114,97,119,118,101,99,95,102,97,108,108,111,102,102,95,109,97,120,91,51,93,0,42,102,95,115,111,117,114,99,101,0,112,100,101,102,95,99,102,114,105,99,116,0,119,101,105,103,104,116,91,49,52,93,0,103,108,111,98,97,108,95,103,114,97,118,105,116,
121,0,116,111,116,115,112,114,105,110,103,0,42,98,112,111,105,110,116,0,42,98,115,112,114,105,110,103,0,109,115,103,95,108,111,99,107,0,109,115,103,95,118,97,108,117,101,0,110,111,100,101,109,97,115,115,0,110,97,109,101,100,86,71,95,77,97,115,115,91,54,52,93,0,103,114,97,118,0,109,101,100,105,97,102,114,105,99,116,0,114,107,108,105,109,105,116,0,112,104,121,115,105,99,115,95,115,112,101,101,100,0,110,97,109,101,100,86,71,95,
83,111,102,116,103,111,97,108,91,54,52,93,0,102,117,122,122,121,110,101,115,115,0,105,110,115,112,114,105,110,103,0,105,110,102,114,105,99,116,0,110,97,109,101,100,86,71,95,83,112,114,105,110,103,95,75,91,54,52,93,0,115,111,108,118,101,114,102,108,97,103,115,0,42,42,107,101,121,115,0,116,111,116,112,111,105,110,116,107,101,121,0,115,101,99,111,110,100,115,112,114,105,110,103,0,99,111,108,98,97,108,108,0,98,97,108,108,100,97,109,
112,0,98,97,108,108,115,116,105,102,102,0,115,98,99,95,109,111,100,101,0,97,101,114,111,101,100,103,101,0,109,105,110,108,111,111,112,115,0,109,97,120,108,111,111,112,115,0,99,104,111,107,101,0,115,111,108,118,101,114,95,73,68,0,112,108,97,115,116,105,99,0,115,112,114,105,110,103,112,114,101,108,111,97,100,0,42,115,99,114,97,116,99,104,0,115,104,101,97,114,115,116,105,102,102,0,105,110,112,117,115,104,0,42,115,104,97,114,101,100,
0,42,99,111,108,108,105,115,105,111,110,95,103,114,111,117,112,0,108,99,111,109,91,51,93,0,108,114,111,116,91,51,93,91,51,93,0,108,115,99,97,108,101,91,51,93,91,51,93,0,108,97,115,116,95,102,114,97,109,101,0,118,101,99,91,56,93,91,51,93,0,117,115,97,103,101,0,108,105,103,104,116,95,115,101,116,95,109,101,109,98,101,114,115,104,105,112,0,115,104,97,100,111,119,95,115,101,116,95,109,101,109,98,101,114,115,104,105,112,0,
114,101,99,101,105,118,101,114,95,108,105,103,104,116,95,115,101,116,0,98,108,111,99,107,101,114,95,115,104,97,100,111,119,95,115,101,116,0,42,114,101,99,101,105,118,101,114,95,99,111,108,108,101,99,116,105,111,110,0,42,98,108,111,99,107,101,114,95,99,111,108,108,101,99,116,105,111,110,0,42,115,99,117,108,112,116,0,112,97,114,49,0,112,97,114,50,0,112,97,114,51,0,42,112,114,111,120,121,0,42,112,114,111,120,121,95,103,114,111,117,
112,0,42,112,114,111,120,121,95,102,114,111,109,0,42,112,111,115,101,108,105,98,0,42,112,111,115,101,0,100,101,102,98,97,115,101,0,102,109,97,112,115,0,103,114,101,97,115,101,112,101,110,99,105,108,95,109,111,100,105,102,105,101,114,115,0,115,104,97,100,101,114,95,102,120,0,114,101,115,116,111,114,101,95,109,111,100,101,0,42,109,97,116,98,105,116,115,0,97,99,116,99,111,108,0,100,108,111,99,91,51,93,0,100,115,105,122,101,91,51,
93,0,100,115,99,97,108,101,91,51,93,0,100,114,111,116,91,51,93,0,100,113,117,97,116,91,52,93,0,100,114,111,116,65,120,105,115,91,51,93,0,100,114,111,116,65,110,103,108,101,0,99,111,108,98,105,116,115,0,116,114,97,110,115,102,108,97,103,0,110,108,97,102,108,97,103,0,100,117,112,108,105,99,97,116,111,114,95,118,105,115,105,98,105,108,105,116,121,95,102,108,97,103,0,98,97,115,101,95,102,108,97,103,0,98,97,115,101,95,108,
111,99,97,108,95,118,105,101,119,95,98,105,116,115,0,99,111,108,95,103,114,111,117,112,0,99,111,108,95,109,97,115,107,0,98,111,117,110,100,116,121,112,101,0,99,111,108,108,105,115,105,111,110,95,98,111,117,110,100,116,121,112,101,0,101,109,112,116,121,95,100,114,97,119,116,121,112,101,0,101,109,112,116,121,95,100,114,97,119,115,105,122,101,0,100,117,112,102,97,99,101,115,99,97,0,97,99,116,100,101,102,0,115,111,102,116,102,108,97,103,
0,115,104,97,112,101,110,114,0,115,104,97,112,101,102,108,97,103,0,95,112,97,100,51,91,49,93,0,110,108,97,115,116,114,105,112,115,0,104,111,111,107,115,0,112,97,114,116,105,99,108,101,115,121,115,116,101,109,0,42,112,100,0,42,115,111,102,116,0,42,100,117,112,95,103,114,111,117,112,0,42,102,108,117,105,100,115,105,109,83,101,116,116,105,110,103,115,0,112,99,95,105,100,115,0,42,114,105,103,105,100,98,111,100,121,95,111,98,106,101,
99,116,0,42,114,105,103,105,100,98,111,100,121,95,99,111,110,115,116,114,97,105,110,116,0,105,109,97,95,111,102,115,91,50,93,0,42,105,117,115,101,114,0,101,109,112,116,121,95,105,109,97,103,101,95,118,105,115,105,98,105,108,105,116,121,95,102,108,97,103,0,101,109,112,116,121,95,105,109,97,103,101,95,100,101,112,116,104,0,101,109,112,116,121,95,105,109,97,103,101,95,102,108,97,103,0,109,111,100,105,102,105,101,114,95,102,108,97,103,0,
115,104,97,100,111,119,95,116,101,114,109,105,110,97,116,111,114,95,110,111,114,109,97,108,95,111,102,102,115,101,116,0,115,104,97,100,111,119,95,116,101,114,109,105,110,97,116,111,114,95,103,101,111,109,101,116,114,121,95,111,102,102,115,101,116,0,115,104,97,100,111,119,95,116,101,114,109,105,110,97,116,111,114,95,115,104,97,100,105,110,103,95,111,102,102,115,101,116,0,42,108,105,103,104,116,103,114,111,117,112,0,42,108,105,103,104,116,95,108,105,
110,107,105,110,103,0,42,108,105,103,104,116,112,114,111,98,101,95,99,97,99,104,101,0,99,117,114,105,110,100,101,120,0,117,115,101,100,0,117,115,101,100,101,108,101,109,0,115,101,101,107,0,119,111,114,108,100,95,99,111,91,51,93,0,114,111,116,91,52,93,0,97,118,101,91,51,93,0,42,103,114,111,117,110,100,0,119,97,110,100,101,114,91,51,93,0,114,101,115,116,95,108,101,110,103,116,104,0,112,97,114,116,105,99,108,101,95,105,110,100,
101,120,91,50,93,0,100,101,108,101,116,101,95,102,108,97,103,0,110,117,109,0,112,97,91,52,93,0,119,91,52,93,0,102,117,118,91,52,93,0,102,111,102,102,115,101,116,0,100,117,114,97,116,105,111,110,0,115,116,97,116,101,0,112,114,101,118,95,115,116,97,116,101,0,42,104,97,105,114,0,42,98,111,105,100,0,100,105,101,116,105,109,101,0,110,117,109,95,100,109,99,97,99,104,101,0,115,112,104,100,101,110,115,105,116,121,0,104,97,105,
114,95,105,110,100,101,120,0,97,108,105,118,101,0,115,112,114,105,110,103,95,107,0,112,108,97,115,116,105,99,105,116,121,95,99,111,110,115,116,97,110,116,0,121,105,101,108,100,95,114,97,116,105,111,0,112,108,97,115,116,105,99,105,116,121,95,98,97,108,97,110,99,101,0,121,105,101,108,100,95,98,97,108,97,110,99,101,0,118,105,115,99,111,115,105,116,121,95,111,109,101,103,97,0,118,105,115,99,111,115,105,116,121,95,98,101,116,97,0,115,
116,105,102,102,110,101,115,115,95,107,0,115,116,105,102,102,110,101,115,115,95,107,110,101,97,114,0,114,101,115,116,95,100,101,110,115,105,116,121,0,98,117,111,121,97,110,99,121,0,115,112,114,105,110,103,95,102,114,97,109,101,115,0,42,98,111,105,100,115,0,100,105,115,116,114,0,112,104,121,115,116,121,112,101,0,97,118,101,109,111,100,101,0,114,101,97,99,116,101,118,101,110,116,0,100,114,97,119,0,100,114,97,119,95,115,105,122,101,0,100,
114,97,119,95,97,115,0,99,104,105,108,100,116,121,112,101,0,114,101,110,95,97,115,0,100,114,97,119,95,99,111,108,0,100,114,97,119,95,115,116,101,112,0,114,101,110,95,115,116,101,112,0,104,97,105,114,95,115,116,101,112,0,107,101,121,115,95,115,116,101,112,0,97,100,97,112,116,95,97,110,103,108,101,0,97,100,97,112,116,95,112,105,120,0,105,110,116,101,103,114,97,116,111,114,0,114,111,116,102,114,111,109,0,98,98,95,97,108,105,103,
110,0,98,98,95,117,118,95,115,112,108,105,116,0,98,98,95,97,110,105,109,0,98,98,95,115,112,108,105,116,95,111,102,102,115,101,116,0,98,98,95,116,105,108,116,0,98,98,95,114,97,110,100,95,116,105,108,116,0,98,98,95,111,102,102,115,101,116,91,50,93,0,98,98,95,115,105,122,101,91,50,93,0,98,98,95,118,101,108,95,104,101,97,100,0,98,98,95,118,101,108,95,116,97,105,108,0,99,111,108,111,114,95,118,101,99,95,109,97,120,
0,116,105,109,101,116,119,101,97,107,0,99,111,117,114,97,110,116,95,116,97,114,103,101,116,0,106,105,116,102,97,99,0,101,102,102,95,104,97,105,114,0,103,114,105,100,95,114,97,110,100,0,112,115,95,111,102,102,115,101,116,91,49,93,0,103,114,105,100,95,114,101,115,0,101,102,102,101,99,116,111,114,95,97,109,111,117,110,116,0,116,105,109,101,95,102,108,97,103,0,112,97,114,116,102,97,99,0,116,97,110,102,97,99,0,116,97,110,112,104,
97,115,101,0,114,101,97,99,116,102,97,99,0,111,98,95,118,101,108,91,51,93,0,97,118,101,102,97,99,0,112,104,97,115,101,102,97,99,0,114,97,110,100,114,111,116,102,97,99,0,114,97,110,100,112,104,97,115,101,102,97,99,0,114,97,110,100,115,105,122,101,0,100,114,97,103,102,97,99,0,98,114,111,119,110,102,97,99,0,100,97,109,112,102,97,99,0,114,97,110,100,108,101,110,103,116,104,0,99,104,105,108,100,95,102,108,97,103,0,99,
104,105,108,100,95,110,98,114,0,114,101,110,95,99,104,105,108,100,95,110,98,114,0,99,104,105,108,100,115,105,122,101,0,99,104,105,108,100,114,97,110,100,115,105,122,101,0,99,104,105,108,100,114,97,100,0,99,104,105,108,100,102,108,97,116,0,99,108,117,109,112,102,97,99,0,99,108,117,109,112,112,111,119,0,107,105,110,107,95,102,108,97,116,0,107,105,110,107,95,97,109,112,95,99,108,117,109,112,0,107,105,110,107,95,101,120,116,114,97,95,
115,116,101,112,115,0,107,105,110,107,95,97,120,105,115,95,114,97,110,100,111,109,0,107,105,110,107,95,97,109,112,95,114,97,110,100,111,109,0,114,111,117,103,104,49,0,114,111,117,103,104,49,95,115,105,122,101,0,114,111,117,103,104,50,0,114,111,117,103,104,50,95,115,105,122,101,0,114,111,117,103,104,50,95,116,104,114,101,115,0,114,111,117,103,104,95,101,110,100,0,114,111,117,103,104,95,101,110,100,95,115,104,97,112,101,0,99,108,101,110,
103,116,104,0,99,108,101,110,103,116,104,95,116,104,114,101,115,0,112,97,114,116,105,110,103,95,102,97,99,0,112,97,114,116,105,110,103,95,109,105,110,0,112,97,114,116,105,110,103,95,109,97,120,0,98,114,97,110,99,104,95,116,104,114,101,115,0,100,114,97,119,95,108,105,110,101,91,50,93,0,112,97,116,104,95,115,116,97,114,116,0,112,97,116,104,95,101,110,100,0,116,114,97,105,108,95,99,111,117,110,116,0,107,101,121,101,100,95,108,111,
111,112,115,0,42,99,108,117,109,112,99,117,114,118,101,0,42,114,111,117,103,104,99,117,114,118,101,0,99,108,117,109,112,95,110,111,105,115,101,95,115,105,122,101,0,98,101,110,100,105,110,103,95,114,97,110,100,111,109,0,100,117,112,108,105,119,101,105,103,104,116,115,0,42,100,117,112,95,111,98,0,42,98,98,95,111,98,0,42,112,100,50,0,117,115,101,95,109,111,100,105,102,105,101,114,95,115,116,97,99,107,0,95,112,97,100,53,91,50,93,
0,115,104,97,112,101,95,102,108,97,103,0,116,119,105,115,116,0,95,112,97,100,56,91,52,93,0,114,97,100,95,114,111,111,116,0,114,97,100,95,116,105,112,0,114,97,100,95,115,99,97,108,101,0,42,116,119,105,115,116,99,117,114,118,101,0,42,95,112,97,100,55,0,42,112,97,114,116,0,42,112,97,114,116,105,99,108,101,115,0,42,101,100,105,116,0,40,42,102,114,101,101,95,101,100,105,116,41,40,41,0,42,42,112,97,116,104,99,97,99,
104,101,0,42,42,99,104,105,108,100,99,97,99,104,101,0,112,97,116,104,99,97,99,104,101,98,117,102,115,0,99,104,105,108,100,99,97,99,104,101,98,117,102,115,0,42,99,108,109,100,0,42,104,97,105,114,95,105,110,95,109,101,115,104,0,42,104,97,105,114,95,111,117,116,95,109,101,115,104,0,42,116,97,114,103,101,116,95,111,98,0,42,108,97,116,116,105,99,101,95,100,101,102,111,114,109,95,100,97,116,97,0,116,114,101,101,95,102,114,97,
109,101,0,98,118,104,116,114,101,101,95,102,114,97,109,101,0,99,104,105,108,100,95,115,101,101,100,0,116,111,116,117,110,101,120,105,115,116,0,116,111,116,99,104,105,108,100,0,116,111,116,99,97,99,104,101,100,0,116,111,116,99,104,105,108,100,99,97,99,104,101,0,116,97,114,103,101,116,95,112,115,121,115,0,116,111,116,107,101,121,101,100,0,98,97,107,101,115,112,97,99,101,0,98,98,95,117,118,110,97,109,101,91,51,93,91,54,56,93,0,
118,103,114,111,117,112,91,49,51,93,0,118,103,95,110,101,103,0,114,116,51,0,95,112,97,100,51,91,54,93,0,42,101,102,102,101,99,116,111,114,115,0,42,102,108,117,105,100,95,115,112,114,105,110,103,115,0,116,111,116,95,102,108,117,105,100,115,112,114,105,110,103,115,0,97,108,108,111,99,95,102,108,117,105,100,115,112,114,105,110,103,115,0,42,116,114,101,101,0,42,112,100,100,0,100,116,95,102,114,97,99,0,108,97,116,116,105,99,101,95,
115,116,114,101,110,103,116,104,0,42,111,114,105,103,95,112,115,121,115,0,116,111,116,100,97,116,97,0,42,100,97,116,97,91,56,93,0,101,120,116,114,97,100,97,116,97,0,115,105,109,102,114,97,109,101,0,115,116,97,114,116,102,114,97,109,101,0,101,110,100,102,114,97,109,101,0,101,100,105,116,102,114,97,109,101,0,108,97,115,116,95,101,120,97,99,116,0,108,97,115,116,95,118,97,108,105,100,0,112,114,101,118,95,110,97,109,101,91,54,52,
93,0,42,99,97,99,104,101,100,95,102,114,97,109,101,115,0,99,97,99,104,101,100,95,102,114,97,109,101,115,95,108,101,110,0,109,101,109,95,99,97,99,104,101,0,95,112,97,100,51,91,51,93,0,42,42,111,98,106,101,99,116,115,0,42,99,111,110,115,116,114,97,105,110,116,115,0,108,116,105,109,101,0,110,117,109,98,111,100,105,101,115,0,115,116,101,112,115,95,112,101,114,95,115,101,99,111,110,100,0,110,117,109,95,115,111,108,118,101,114,
95,105,116,101,114,97,116,105,111,110,115,0,99,111,108,95,103,114,111,117,112,115,0,109,101,115,104,95,115,111,117,114,99,101,0,114,101,115,116,105,116,117,116,105,111,110,0,109,97,114,103,105,110,0,108,105,110,95,100,97,109,112,105,110,103,0,97,110,103,95,100,97,109,112,105,110,103,0,108,105,110,95,115,108,101,101,112,95,116,104,114,101,115,104,0,97,110,103,95,115,108,101,101,112,95,116,104,114,101,115,104,0,111,114,110,91,52,93,0,112,
111,115,91,51,93,0,42,111,98,49,0,42,111,98,50,0,98,114,101,97,107,105,110,103,95,116,104,114,101,115,104,111,108,100,0,115,112,114,105,110,103,95,116,121,112,101,0,108,105,109,105,116,95,108,105,110,95,120,95,108,111,119,101,114,0,108,105,109,105,116,95,108,105,110,95,120,95,117,112,112,101,114,0,108,105,109,105,116,95,108,105,110,95,121,95,108,111,119,101,114,0,108,105,109,105,116,95,108,105,110,95,121,95,117,112,112,101,114,0,108,
105,109,105,116,95,108,105,110,95,122,95,108,111,119,101,114,0,108,105,109,105,116,95,108,105,110,95,122,95,117,112,112,101,114,0,108,105,109,105,116,95,97,110,103,95,120,95,108,111,119,101,114,0,108,105,109,105,116,95,97,110,103,95,120,95,117,112,112,101,114,0,108,105,109,105,116,95,97,110,103,95,121,95,108,111,119,101,114,0,108,105,109,105,116,95,97,110,103,95,121,95,117,112,112,101,114,0,108,105,109,105,116,95,97,110,103,95,122,95,108,
111,119,101,114,0,108,105,109,105,116,95,97,110,103,95,122,95,117,112,112,101,114,0,115,112,114,105,110,103,95,115,116,105,102,102,110,101,115,115,95,120,0,115,112,114,105,110,103,95,115,116,105,102,102,110,101,115,115,95,121,0,115,112,114,105,110,103,95,115,116,105,102,102,110,101,115,115,95,122,0,115,112,114,105,110,103,95,115,116,105,102,102,110,101,115,115,95,97,110,103,95,120,0,115,112,114,105,110,103,95,115,116,105,102,102,110,101,115,115,95,
97,110,103,95,121,0,115,112,114,105,110,103,95,115,116,105,102,102,110,101,115,115,95,97,110,103,95,122,0,115,112,114,105,110,103,95,100,97,109,112,105,110,103,95,120,0,115,112,114,105,110,103,95,100,97,109,112,105,110,103,95,121,0,115,112,114,105,110,103,95,100,97,109,112,105,110,103,95,122,0,115,112,114,105,110,103,95,100,97,109,112,105,110,103,95,97,110,103,95,120,0,115,112,114,105,110,103,95,100,97,109,112,105,110,103,95,97,110,103,95,
121,0,115,112,114,105,110,103,95,100,97,109,112,105,110,103,95,97,110,103,95,122,0,109,111,116,111,114,95,108,105,110,95,116,97,114,103,101,116,95,118,101,108,111,99,105,116,121,0,109,111,116,111,114,95,97,110,103,95,116,97,114,103,101,116,95,118,101,108,111,99,105,116,121,0,109,111,116,111,114,95,108,105,110,95,109,97,120,95,105,109,112,117,108,115,101,0,109,111,116,111,114,95,97,110,103,95,109,97,120,95,105,109,112,117,108,115,101,0,42,
112,104,121,115,105,99,115,95,99,111,110,115,116,114,97,105,110,116,0,99,111,100,101,99,0,97,117,100,105,111,95,99,111,100,101,99,0,118,105,100,101,111,95,98,105,116,114,97,116,101,0,97,117,100,105,111,95,98,105,116,114,97,116,101,0,97,117,100,105,111,95,109,105,120,114,97,116,101,0,97,117,100,105,111,95,99,104,97,110,110,101,108,115,0,97,117,100,105,111,95,118,111,108,117,109,101,0,103,111,112,95,115,105,122,101,0,109,97,120,95,
98,95,102,114,97,109,101,115,0,99,111,110,115,116,97,110,116,95,114,97,116,101,95,102,97,99,116,111,114,0,102,102,109,112,101,103,95,112,114,101,115,101,116,0,102,102,109,112,101,103,95,112,114,111,114,101,115,95,112,114,111,102,105,108,101,0,114,99,95,109,105,110,95,114,97,116,101,0,114,99,95,109,97,120,95,114,97,116,101,0,114,99,95,98,117,102,102,101,114,95,115,105,122,101,0,109,117,120,95,112,97,99,107,101,116,95,115,105,122,101,
0,109,117,120,95,114,97,116,101,0,109,105,120,114,97,116,101,0,109,97,105,110,0,115,112,101,101,100,95,111,102,95,115,111,117,110,100,0,100,111,112,112,108,101,114,95,102,97,99,116,111,114,0,100,105,115,116,97,110,99,101,95,109,111,100,101,108,0,108,97,121,95,122,109,97,115,107,0,108,97,121,95,101,120,99,108,117,100,101,0,112,97,115,115,95,120,111,114,0,102,114,101,101,115,116,121,108,101,67,111,110,102,105,103,0,115,117,102,102,105,
120,91,54,52,93,0,118,105,101,119,102,108,97,103,0,100,105,115,112,108,97,121,95,109,111,100,101,0,97,110,97,103,108,121,112,104,95,116,121,112,101,0,105,110,116,101,114,108,97,99,101,95,116,121,112,101,0,105,109,116,121,112,101,0,112,108,97,110,101,115,0,99,111,109,112,114,101,115,115,0,101,120,114,95,99,111,100,101,99,0,99,105,110,101,111,110,95,102,108,97,103,0,99,105,110,101,111,110,95,119,104,105,116,101,0,99,105,110,101,111,
110,95,98,108,97,99,107,0,99,105,110,101,111,110,95,103,97,109,109,97,0,106,112,50,95,102,108,97,103,0,106,112,50,95,99,111,100,101,99,0,116,105,102,102,95,99,111,100,101,99,0,115,116,101,114,101,111,51,100,95,102,111,114,109,97,116,0,99,111,108,111,114,95,109,97,110,97,103,101,109,101,110,116,0,118,105,101,119,95,115,101,116,116,105,110,103,115,0,100,105,115,112,108,97,121,95,115,101,116,116,105,110,103,115,0,108,105,110,101,97,
114,95,99,111,108,111,114,115,112,97,99,101,95,115,101,116,116,105,110,103,115,0,99,97,103,101,95,101,120,116,114,117,115,105,111,110,0,109,97,120,95,114,97,121,95,100,105,115,116,97,110,99,101,0,112,97,115,115,95,102,105,108,116,101,114,0,110,111,114,109,97,108,95,115,119,105,122,122,108,101,91,51,93,0,110,111,114,109,97,108,95,115,112,97,99,101,0,116,97,114,103,101,116,0,115,97,118,101,95,109,111,100,101,0,109,97,114,103,105,110,
95,116,121,112,101,0,118,105,101,119,95,102,114,111,109,0,42,99,97,103,101,95,111,98,106,101,99,116,0,102,102,99,111,100,101,99,100,97,116,97,0,115,117,98,102,114,97,109,101,0,112,115,102,114,97,0,112,101,102,114,97,0,105,109,97,103,101,115,0,102,114,97,109,97,112,116,111,0,102,114,97,109,101,108,101,110,0,102,114,97,109,101,95,115,116,101,112,0,100,105,109,101,110,115,105,111,110,115,112,114,101,115,101,116,0,120,115,99,104,0,
121,115,99,104,0,116,105,108,101,120,0,116,105,108,101,121,0,115,117,98,105,109,116,121,112,101,0,117,115,101,95,108,111,99,107,95,105,110,116,101,114,102,97,99,101,0,95,112,97,100,55,91,51,93,0,115,99,101,109,111,100,101,0,102,114,115,95,115,101,99,0,97,108,112,104,97,109,111,100,101,0,95,112,97,100,48,91,49,93,0,98,111,114,100,101,114,0,97,99,116,108,97,121,0,120,97,115,112,0,121,97,115,112,0,112,112,109,95,102,97,
99,116,111,114,0,112,112,109,95,98,97,115,101,0,102,114,115,95,115,101,99,95,98,97,115,101,0,103,97,117,115,115,0,99,111,108,111,114,95,109,103,116,95,102,108,97,103,0,100,105,116,104,101,114,95,105,110,116,101,110,115,105,116,121,0,98,97,107,101,95,102,108,97,103,0,98,97,107,101,95,102,105,108,116,101,114,0,98,97,107,101,95,115,97,109,112,108,101,115,0,98,97,107,101,95,109,97,114,103,105,110,95,116,121,112,101,0,95,112,97,
100,57,91,54,93,0,98,97,107,101,95,98,105,97,115,100,105,115,116,0,98,97,107,101,95,117,115,101,114,95,115,99,97,108,101,0,112,105,99,91,49,48,50,52,93,0,115,116,97,109,112,0,115,116,97,109,112,95,102,111,110,116,95,105,100,0,115,116,97,109,112,95,117,100,97,116,97,91,55,54,56,93,0,102,103,95,115,116,97,109,112,91,52,93,0,98,103,95,115,116,97,109,112,91,52,93,0,115,101,113,95,112,114,101,118,95,116,121,112,101,
0,115,101,113,95,114,101,110,100,95,116,121,112,101,0,115,101,113,95,102,108,97,103,0,95,112,97,100,53,91,51,93,0,115,105,109,112,108,105,102,121,95,115,117,98,115,117,114,102,0,115,105,109,112,108,105,102,121,95,115,117,98,115,117,114,102,95,114,101,110,100,101,114,0,115,105,109,112,108,105,102,121,95,103,112,101,110,99,105,108,0,115,105,109,112,108,105,102,121,95,112,97,114,116,105,99,108,101,115,0,115,105,109,112,108,105,102,121,95,112,
97,114,116,105,99,108,101,115,95,114,101,110,100,101,114,0,115,105,109,112,108,105,102,121,95,118,111,108,117,109,101,115,0,108,105,110,101,95,116,104,105,99,107,110,101,115,115,95,109,111,100,101,0,117,110,105,116,95,108,105,110,101,95,116,104,105,99,107,110,101,115,115,0,101,110,103,105,110,101,91,51,50,93,0,112,101,114,102,95,102,108,97,103,0,98,97,107,101,0,95,112,97,100,56,0,112,114,101,118,105,101,119,95,112,105,120,101,108,95,115,
105,122,101,0,97,99,116,118,105,101,119,0,104,97,105,114,95,116,121,112,101,0,104,97,105,114,95,115,117,98,100,105,118,0,98,108,117,114,102,97,99,0,109,111,116,105,111,110,95,98,108,117,114,95,112,111,115,105,116,105,111,110,0,109,98,108,117,114,95,115,104,117,116,116,101,114,95,99,117,114,118,101,0,99,111,109,112,111,115,105,116,111,114,95,100,101,118,105,99,101,0,99,111,109,112,111,115,105,116,111,114,95,112,114,101,99,105,115,105,111,
110,0,99,111,109,112,111,115,105,116,111,114,95,100,101,110,111,105,115,101,95,100,101,118,105,99,101,0,99,111,109,112,111,115,105,116,111,114,95,100,101,110,111,105,115,101,95,112,114,101,118,105,101,119,95,113,117,97,108,105,116,121,0,99,111,109,112,111,115,105,116,111,114,95,100,101,110,111,105,115,101,95,102,105,110,97,108,95,113,117,97,108,105,116,121,0,95,112,97,100,54,91,52,93,0,105,110,105,116,105,97,108,105,122,101,100,0,42,112,114,
101,118,105,111,117,115,95,97,99,116,105,118,101,95,98,114,117,115,104,95,114,101,102,101,114,101,110,99,101,0,42,98,114,117,115,104,95,97,115,115,101,116,95,114,101,102,101,114,101,110,99,101,0,42,109,97,105,110,95,98,114,117,115,104,95,97,115,115,101,116,95,114,101,102,101,114,101,110,99,101,0,97,99,116,105,118,101,95,98,114,117,115,104,95,112,101,114,95,98,114,117,115,104,95,116,121,112,101,0,42,101,114,97,115,101,114,95,98,114,117,
115,104,0,42,101,114,97,115,101,114,95,98,114,117,115,104,95,97,115,115,101,116,95,114,101,102,101,114,101,110,99,101,0,116,111,111,108,95,98,114,117,115,104,95,98,105,110,100,105,110,103,115,0,42,112,97,108,101,116,116,101,0,42,99,97,118,105,116,121,95,99,117,114,118,101,0,42,112,97,105,110,116,95,99,117,114,115,111,114,0,112,97,105,110,116,95,99,117,114,115,111,114,95,99,111,108,91,52,93,0,110,117,109,95,105,110,112,117,116,95,
115,97,109,112,108,101,115,0,115,121,109,109,101,116,114,121,95,102,108,97,103,115,0,116,105,108,101,95,111,102,102,115,101,116,91,51,93,0,112,97,105,110,116,0,109,105,115,115,105,110,103,95,100,97,116,97,0,115,101,97,109,95,98,108,101,101,100,0,110,111,114,109,97,108,95,97,110,103,108,101,0,115,99,114,101,101,110,95,103,114,97,98,95,115,105,122,101,91,50,93,0,42,115,116,101,110,99,105,108,0,42,99,108,111,110,101,0,115,116,101,
110,99,105,108,95,99,111,108,91,51,93,0,100,105,116,104,101,114,0,99,108,111,110,101,95,111,102,102,115,101,116,91,50,93,0,99,108,111,110,101,95,97,108,112,104,97,0,99,97,110,118,97,115,95,115,111,117,114,99,101,0,42,99,97,110,118,97,115,95,105,109,97,103,101,0,105,109,97,103,101,95,117,115,101,114,0,105,110,118,101,114,116,0,116,111,116,114,101,107,101,121,0,116,111,116,97,100,100,107,101,121,0,98,114,117,115,104,116,121,112,
101,0,98,114,117,115,104,91,55,93,0,42,112,97,105,110,116,99,117,114,115,111,114,0,101,109,105,116,116,101,114,100,105,115,116,0,115,101,108,101,99,116,109,111,100,101,0,101,100,105,116,116,121,112,101,0,102,97,100,101,95,102,114,97,109,101,115,0,42,115,104,97,112,101,95,111,98,106,101,99,116,0,116,114,97,110,115,102,111,114,109,95,109,111,100,101,0,114,97,100,105,97,108,95,115,121,109,109,91,51,93,0,100,101,116,97,105,108,95,115,
105,122,101,0,115,121,109,109,101,116,114,105,122,101,95,100,105,114,101,99,116,105,111,110,0,103,114,97,118,105,116,121,95,102,97,99,116,111,114,0,99,111,110,115,116,97,110,116,95,100,101,116,97,105,108,0,100,101,116,97,105,108,95,112,101,114,99,101,110,116,0,42,97,117,116,111,109,97,115,107,105,110,103,95,99,97,118,105,116,121,95,99,117,114,118,101,95,111,112,0,42,103,114,97,118,105,116,121,95,111,98,106,101,99,116,0,42,115,116,114,
101,110,103,116,104,95,99,117,114,118,101,0,117,115,101,95,103,117,105,100,101,0,117,115,101,95,115,110,97,112,112,105,110,103,0,114,101,102,101,114,101,110,99,101,95,112,111,105,110,116,0,97,110,103,108,101,95,115,110,97,112,0,42,114,101,102,101,114,101,110,99,101,95,111,98,106,101,99,116,0,108,111,99,107,95,97,120,105,115,0,105,115,101,99,116,95,116,104,114,101,115,104,111,108,100,0,42,99,117,114,95,102,97,108,108,111,102,102,0,42,
99,117,114,95,112,114,105,109,105,116,105,118,101,0,103,117,105,100,101,0,42,99,117,115,116,111,109,95,105,112,111,0,108,97,115,116,95,114,97,107,101,91,50,93,0,108,97,115,116,95,114,97,107,101,95,97,110,103,108,101,0,108,97,115,116,95,115,116,114,111,107,101,95,118,97,108,105,100,0,97,118,101,114,97,103,101,95,115,116,114,111,107,101,95,97,99,99,117,109,91,51,93,0,97,118,101,114,97,103,101,95,115,116,114,111,107,101,95,99,111,
117,110,116,101,114,0,98,114,117,115,104,95,114,111,116,97,116,105,111,110,0,98,114,117,115,104,95,114,111,116,97,116,105,111,110,95,115,101,99,0,97,110,99,104,111,114,101,100,95,115,105,122,101,0,111,118,101,114,108,97,112,95,102,97,99,116,111,114,0,100,114,97,119,95,105,110,118,101,114,116,101,100,0,115,116,114,111,107,101,95,97,99,116,105,118,101,0,100,114,97,119,95,97,110,99,104,111,114,101,100,0,100,111,95,108,105,110,101,97,114,
95,99,111,110,118,101,114,115,105,111,110,0,108,97,115,116,95,108,111,99,97,116,105,111,110,91,51,93,0,108,97,115,116,95,104,105,116,0,97,110,99,104,111,114,101,100,95,105,110,105,116,105,97,108,95,109,111,117,115,101,91,50,93,0,112,105,120,101,108,95,114,97,100,105,117,115,0,105,110,105,116,105,97,108,95,112,105,120,101,108,95,114,97,100,105,117,115,0,115,116,97,114,116,95,112,105,120,101,108,95,114,97,100,105,117,115,0,115,105,122,
101,95,112,114,101,115,115,117,114,101,95,118,97,108,117,101,0,116,101,120,95,109,111,117,115,101,91,50,93,0,109,97,115,107,95,116,101,120,95,109,111,117,115,101,91,50,93,0,42,99,111,108,111,114,115,112,97,99,101,0,99,117,114,118,101,95,116,121,112,101,0,100,101,112,116,104,95,109,111,100,101,0,115,117,114,102,97,99,101,95,112,108,97,110,101,0,101,114,114,111,114,95,116,104,114,101,115,104,111,108,100,0,114,97,100,105,117,115,95,109,
105,110,0,114,97,100,105,117,115,95,109,97,120,0,114,97,100,105,117,115,95,116,97,112,101,114,95,115,116,97,114,116,0,114,97,100,105,117,115,95,116,97,112,101,114,95,101,110,100,0,115,117,114,102,97,99,101,95,111,102,102,115,101,116,0,99,111,114,110,101,114,95,97,110,103,108,101,0,111,118,101,114,104,97,110,103,95,97,120,105,115,0,111,118,101,114,104,97,110,103,95,109,105,110,0,111,118,101,114,104,97,110,103,95,109,97,120,0,116,104,
105,99,107,110,101,115,115,95,109,105,110,0,116,104,105,99,107,110,101,115,115,95,109,97,120,0,116,104,105,99,107,110,101,115,115,95,115,97,109,112,108,101,115,0,100,105,115,116,111,114,116,95,109,105,110,0,100,105,115,116,111,114,116,95,109,97,120,0,115,104,97,114,112,95,109,105,110,0,115,104,97,114,112,95,109,97,120,0,115,110,97,112,95,109,111,100,101,0,115,110,97,112,95,102,108,97,103,0,111,118,101,114,108,97,112,95,109,111,100,101,
0,115,110,97,112,95,100,105,115,116,97,110,99,101,0,112,105,118,111,116,95,112,111,105,110,116,0,42,118,112,97,105,110,116,0,42,119,112,97,105,110,116,0,117,118,115,99,117,108,112,116,0,42,103,112,95,112,97,105,110,116,0,42,103,112,95,118,101,114,116,101,120,112,97,105,110,116,0,42,103,112,95,115,99,117,108,112,116,112,97,105,110,116,0,42,103,112,95,119,101,105,103,104,116,112,97,105,110,116,0,42,99,117,114,118,101,115,95,115,99,
117,108,112,116,0,118,103,114,111,117,112,95,119,101,105,103,104,116,0,100,111,117,98,108,105,109,105,116,0,97,117,116,111,109,101,114,103,101,0,111,98,106,101,99,116,95,102,108,97,103,0,117,110,119,114,97,112,112,101,114,0,117,118,99,97,108,99,95,102,108,97,103,0,117,118,95,102,108,97,103,0,117,118,95,115,101,108,101,99,116,109,111,100,101,0,117,118,95,115,116,105,99,107,121,0,117,118,99,97,108,99,95,109,97,114,103,105,110,0,117,
118,99,97,108,99,95,105,116,101,114,97,116,105,111,110,115,0,117,118,99,97,108,99,95,119,101,105,103,104,116,95,102,97,99,116,111,114,0,117,118,99,97,108,99,95,119,101,105,103,104,116,95,103,114,111,117,112,91,54,52,93,0,97,117,116,111,105,107,95,99,104,97,105,110,108,101,110,0,103,112,101,110,99,105,108,95,102,108,97,103,115,0,103,112,101,110,99,105,108,95,118,51,100,95,97,108,105,103,110,0,103,112,101,110,99,105,108,95,118,50,
100,95,97,108,105,103,110,0,97,110,110,111,116,97,116,101,95,118,51,100,95,97,108,105,103,110,0,97,110,110,111,116,97,116,101,95,116,104,105,99,107,110,101,115,115,0,103,112,101,110,99,105,108,95,115,117,114,102,97,99,101,95,111,102,102,115,101,116,0,103,112,101,110,99,105,108,95,115,101,108,101,99,116,109,111,100,101,95,101,100,105,116,0,103,112,101,110,99,105,108,95,115,101,108,101,99,116,109,111,100,101,95,115,99,117,108,112,116,0,103,
112,95,115,99,117,108,112,116,0,103,112,95,105,110,116,101,114,112,111,108,97,116,101,0,105,109,97,112,97,105,110,116,0,112,97,105,110,116,95,109,111,100,101,0,112,97,114,116,105,99,108,101,0,112,114,111,112,111,114,116,105,111,110,97,108,95,115,105,122,101,0,115,101,108,101,99,116,95,116,104,114,101,115,104,0,107,101,121,105,110,103,95,102,108,97,103,0,97,117,116,111,107,101,121,95,109,111,100,101,0,107,101,121,102,114,97,109,101,95,116,
121,112,101,0,109,117,108,116,105,114,101,115,95,115,117,98,100,105,118,95,116,121,112,101,0,101,100,103,101,95,109,111,100,101,0,101,100,103,101,95,109,111,100,101,95,108,105,118,101,95,117,110,119,114,97,112,0,116,114,97,110,115,102,111,114,109,95,112,105,118,111,116,95,112,111,105,110,116,0,116,114,97,110,115,102,111,114,109,95,102,108,97,103,0,115,110,97,112,95,110,111,100,101,95,109,111,100,101,0,115,110,97,112,95,117,118,95,109,111,100,
101,0,115,110,97,112,95,97,110,105,109,95,109,111,100,101,0,115,110,97,112,95,112,108,97,121,104,101,97,100,95,109,111,100,101,0,115,110,97,112,95,102,108,97,103,95,110,111,100,101,0,115,110,97,112,95,102,108,97,103,95,115,101,113,0,115,110,97,112,95,102,108,97,103,95,97,110,105,109,0,115,110,97,112,95,102,108,97,103,95,100,114,105,118,101,114,0,115,110,97,112,95,102,108,97,103,95,112,108,97,121,104,101,97,100,0,115,110,97,112,
95,117,118,95,102,108,97,103,0,115,110,97,112,95,116,97,114,103,101,116,0,115,110,97,112,95,116,114,97,110,115,102,111,114,109,95,109,111,100,101,95,102,108,97,103,0,115,110,97,112,95,102,97,99,101,95,110,101,97,114,101,115,116,95,115,116,101,112,115,0,112,114,111,112,111,114,116,105,111,110,97,108,95,101,100,105,116,0,112,114,111,112,95,109,111,100,101,0,112,114,111,112,111,114,116,105,111,110,97,108,95,111,98,106,101,99,116,115,0,112,
114,111,112,111,114,116,105,111,110,97,108,95,109,97,115,107,0,112,114,111,112,111,114,116,105,111,110,97,108,95,97,99,116,105,111,110,0,112,114,111,112,111,114,116,105,111,110,97,108,95,102,99,117,114,118,101,0,108,111,99,107,95,109,97,114,107,101,114,115,0,97,117,116,111,95,110,111,114,109,97,108,105,122,101,0,119,112,97,105,110,116,95,108,111,99,107,95,114,101,108,97,116,105,118,101,0,109,117,108,116,105,112,97,105,110,116,0,119,101,105,
103,104,116,117,115,101,114,0,118,103,114,111,117,112,115,117,98,115,101,116,0,103,112,101,110,99,105,108,95,115,101,108,101,99,116,109,111,100,101,95,118,101,114,116,101,120,0,117,118,95,115,99,117,108,112,116,95,115,101,116,116,105,110,103,115,0,119,111,114,107,115,112,97,99,101,95,116,111,111,108,95,116,121,112,101,0,95,112,97,100,53,91,55,93,0,115,99,117,108,112,116,95,112,97,105,110,116,95,115,101,116,116,105,110,103,115,0,115,99,117,
108,112,116,95,112,97,105,110,116,95,117,110,105,102,105,101,100,95,115,105,122,101,0,115,99,117,108,112,116,95,112,97,105,110,116,95,117,110,105,102,105,101,100,95,117,110,112,114,111,106,101,99,116,101,100,95,114,97,100,105,117,115,0,115,99,117,108,112,116,95,112,97,105,110,116,95,117,110,105,102,105,101,100,95,97,108,112,104,97,0,117,110,105,102,105,101,100,95,112,97,105,110,116,95,115,101,116,116,105,110,103,115,0,99,117,114,118,101,95,112,
97,105,110,116,95,115,101,116,116,105,110,103,115,0,115,116,97,116,118,105,115,0,110,111,114,109,97,108,95,118,101,99,116,111,114,91,51,93,0,42,99,117,115,116,111,109,95,98,101,118,101,108,95,112,114,111,102,105,108,101,95,112,114,101,115,101,116,0,42,115,101,113,117,101,110,99,101,114,95,116,111,111,108,95,115,101,116,116,105,110,103,115,0,115,110,97,112,95,109,111,100,101,95,116,111,111,108,115,0,112,108,97,110,101,95,97,120,105,115,0,
112,108,97,110,101,95,111,114,105,101,110,116,0,117,115,101,95,112,108,97,110,101,95,97,120,105,115,95,97,117,116,111,0,95,112,97,100,55,91,50,93,0,115,110,97,112,95,97,110,103,108,101,95,105,110,99,114,101,109,101,110,116,95,50,100,0,115,110,97,112,95,97,110,103,108,101,95,105,110,99,114,101,109,101,110,116,95,50,100,95,112,114,101,99,105,115,105,111,110,0,115,110,97,112,95,97,110,103,108,101,95,105,110,99,114,101,109,101,110,116,
95,51,100,0,115,110,97,112,95,97,110,103,108,101,95,105,110,99,114,101,109,101,110,116,95,51,100,95,112,114,101,99,105,115,105,111,110,0,115,110,97,112,95,115,116,101,112,95,115,101,99,111,110,100,115,0,115,110,97,112,95,115,116,101,112,95,102,114,97,109,101,115,0,112,108,97,121,104,101,97,100,95,115,110,97,112,95,100,105,115,116,97,110,99,101,0,115,99,97,108,101,95,108,101,110,103,116,104,0,115,121,115,116,101,109,0,115,121,115,116,
101,109,95,114,111,116,97,116,105,111,110,0,108,101,110,103,116,104,95,117,110,105,116,0,109,97,115,115,95,117,110,105,116,0,116,105,109,101,95,117,110,105,116,0,116,101,109,112,101,114,97,116,117,114,101,95,117,110,105,116,0,113,117,105,99,107,95,99,97,99,104,101,95,115,116,101,112,0,116,105,116,108,101,91,50,93,0,97,99,116,105,111,110,91,50,93,0,116,105,116,108,101,95,99,101,110,116,101,114,91,50,93,0,97,99,116,105,111,110,95,
99,101,110,116,101,114,91,50,93,0,108,105,103,104,116,95,100,105,114,101,99,116,105,111,110,91,51,93,0,115,104,97,100,111,119,95,115,104,105,102,116,0,115,104,97,100,111,119,95,102,111,99,117,115,0,109,97,116,99,97,112,95,115,115,97,111,95,100,105,115,116,97,110,99,101,0,109,97,116,99,97,112,95,115,115,97,111,95,97,116,116,101,110,117,97,116,105,111,110,0,109,97,116,99,97,112,95,115,115,97,111,95,115,97,109,112,108,101,115,0,
118,105,101,119,112,111,114,116,95,97,97,0,114,101,110,100,101,114,95,97,97,0,115,104,97,100,105,110,103,0,115,99,114,101,101,110,95,116,114,97,99,101,95,113,117,97,108,105,116,121,0,115,99,114,101,101,110,95,116,114,97,99,101,95,116,104,105,99,107,110,101,115,115,0,116,114,97,99,101,95,109,97,120,95,114,111,117,103,104,110,101,115,115,0,114,101,115,111,108,117,116,105,111,110,95,115,99,97,108,101,0,100,101,110,111,105,115,101,95,115,
116,97,103,101,115,0,103,105,95,100,105,102,102,117,115,101,95,98,111,117,110,99,101,115,0,103,105,95,99,117,98,101,109,97,112,95,114,101,115,111,108,117,116,105,111,110,0,103,105,95,118,105,115,105,98,105,108,105,116,121,95,114,101,115,111,108,117,116,105,111,110,0,103,105,95,103,108,111,115,115,121,95,99,108,97,109,112,0,103,105,95,105,114,114,97,100,105,97,110,99,101,95,112,111,111,108,95,115,105,122,101,0,116,97,97,95,115,97,109,112,
108,101,115,0,116,97,97,95,114,101,110,100,101,114,95,115,97,109,112,108,101,115,0,118,111,108,117,109,101,116,114,105,99,95,115,116,97,114,116,0,118,111,108,117,109,101,116,114,105,99,95,101,110,100,0,118,111,108,117,109,101,116,114,105,99,95,116,105,108,101,95,115,105,122,101,0,118,111,108,117,109,101,116,114,105,99,95,115,97,109,112,108,101,115,0,118,111,108,117,109,101,116,114,105,99,95,115,97,109,112,108,101,95,100,105,115,116,114,105,98,
117,116,105,111,110,0,118,111,108,117,109,101,116,114,105,99,95,108,105,103,104,116,95,99,108,97,109,112,0,118,111,108,117,109,101,116,114,105,99,95,115,104,97,100,111,119,95,115,97,109,112,108,101,115,0,118,111,108,117,109,101,116,114,105,99,95,114,97,121,95,100,101,112,116,104,0,103,116,97,111,95,100,105,115,116,97,110,99,101,0,103,116,97,111,95,116,104,105,99,107,110,101,115,115,0,103,116,97,111,95,102,111,99,117,115,0,103,116,97,111,
95,114,101,115,111,108,117,116,105,111,110,0,102,97,115,116,95,103,105,95,115,116,101,112,95,99,111,117,110,116,0,102,97,115,116,95,103,105,95,114,97,121,95,99,111,117,110,116,0,103,116,97,111,95,113,117,97,108,105,116,121,0,102,97,115,116,95,103,105,95,100,105,115,116,97,110,99,101,0,102,97,115,116,95,103,105,95,116,104,105,99,107,110,101,115,115,95,110,101,97,114,0,102,97,115,116,95,103,105,95,116,104,105,99,107,110,101,115,115,95,
102,97,114,0,102,97,115,116,95,103,105,95,109,101,116,104,111,100,0,98,111,107,101,104,95,111,118,101,114,98,108,117,114,0,98,111,107,101,104,95,109,97,120,95,115,105,122,101,0,98,111,107,101,104,95,116,104,114,101,115,104,111,108,100,0,98,111,107,101,104,95,110,101,105,103,104,98,111,114,95,109,97,120,0,109,111,116,105,111,110,95,98,108,117,114,95,109,97,120,0,109,111,116,105,111,110,95,98,108,117,114,95,115,116,101,112,115,0,109,111,
116,105,111,110,95,98,108,117,114,95,100,101,112,116,104,95,115,99,97,108,101,0,115,104,97,100,111,119,95,99,117,98,101,95,115,105,122,101,0,115,104,97,100,111,119,95,112,111,111,108,95,115,105,122,101,0,115,104,97,100,111,119,95,114,97,121,95,99,111,117,110,116,0,115,104,97,100,111,119,95,115,116,101,112,95,99,111,117,110,116,0,115,104,97,100,111,119,95,114,101,115,111,108,117,116,105,111,110,95,115,99,97,108,101,0,99,108,97,109,112,
95,115,117,114,102,97,99,101,95,100,105,114,101,99,116,0,99,108,97,109,112,95,115,117,114,102,97,99,101,95,105,110,100,105,114,101,99,116,0,99,108,97,109,112,95,118,111,108,117,109,101,95,100,105,114,101,99,116,0,99,108,97,109,112,95,118,111,108,117,109,101,95,105,110,100,105,114,101,99,116,0,114,97,121,95,116,114,97,99,105,110,103,95,109,101,116,104,111,100,0,114,97,121,95,116,114,97,99,105,110,103,95,111,112,116,105,111,110,115,0,
108,105,103,104,116,95,116,104,114,101,115,104,111,108,100,0,115,109,97,97,95,116,104,114,101,115,104,111,108,100,0,115,109,97,97,95,116,104,114,101,115,104,111,108,100,95,114,101,110,100,101,114,0,97,97,95,115,97,109,112,108,101,115,0,101,120,112,111,114,116,95,109,101,116,104,111,100,0,105,110,100,101,120,95,99,117,115,116,111,109,0,42,119,111,114,108,100,0,42,115,101,116,0,99,117,114,115,111,114,0,108,97,121,97,99,116,0,42,101,100,
0,42,116,111,111,108,115,101,116,116,105,110,103,115,0,115,97,102,101,95,97,114,101,97,115,0,97,117,100,105,111,0,116,114,97,110,115,102,111,114,109,95,115,112,97,99,101,115,0,111,114,105,101,110,116,97,116,105,111,110,95,115,108,111,116,115,91,52,93,0,42,115,111,117,110,100,95,115,99,101,110,101,0,42,112,108,97,121,98,97,99,107,95,104,97,110,100,108,101,0,42,115,111,117,110,100,95,115,99,114,117,98,95,104,97,110,100,108,101,0,
42,115,112,101,97,107,101,114,95,104,97,110,100,108,101,115,0,42,102,112,115,95,105,110,102,111,0,42,100,101,112,115,103,114,97,112,104,95,104,97,115,104,0,95,112,97,100,55,91,52,93,0,97,99,116,105,118,101,95,107,101,121,105,110,103,115,101,116,0,107,101,121,105,110,103,115,101,116,115,0,117,110,105,116,0,112,104,121,115,105,99,115,95,115,101,116,116,105,110,103,115,0,42,95,112,97,100,56,0,99,117,115,116,111,109,100,97,116,97,95,
109,97,115,107,0,99,117,115,116,111,109,100,97,116,97,95,109,97,115,107,95,109,111,100,97,108,0,115,101,113,117,101,110,99,101,114,95,99,111,108,111,114,115,112,97,99,101,95,115,101,116,116,105,110,103,115,0,42,114,105,103,105,100,98,111,100,121,95,119,111,114,108,100,0,118,105,101,119,95,108,97,121,101,114,115,0,42,109,97,115,116,101,114,95,99,111,108,108,101,99,116,105,111,110,0,42,108,97,121,101,114,95,112,114,111,112,101,114,116,105,
101,115,0,115,105,109,117,108,97,116,105,111,110,95,102,114,97,109,101,95,115,116,97,114,116,0,115,105,109,117,108,97,116,105,111,110,95,102,114,97,109,101,95,101,110,100,0,100,105,115,112,108,97,121,0,103,114,101,97,115,101,95,112,101,110,99,105,108,95,115,101,116,116,105,110,103,115,0,104,121,100,114,97,0,42,95,112,97,100,57,0,118,101,114,116,98,97,115,101,0,101,100,103,101,98,97,115,101,0,97,114,101,97,98,97,115,101,0,119,105,
110,105,100,0,114,101,100,114,97,119,115,95,102,108,97,103,0,100,111,95,100,114,97,119,0,100,111,95,114,101,102,114,101,115,104,0,100,111,95,100,114,97,119,95,103,101,115,116,117,114,101,0,100,111,95,100,114,97,119,95,112,97,105,110,116,99,117,114,115,111,114,0,100,111,95,100,114,97,119,95,100,114,97,103,0,115,107,105,112,95,104,97,110,100,108,105,110,103,0,115,99,114,117,98,98,105,110,103,0,42,97,99,116,105,118,101,95,114,101,103,
105,111,110,0,42,97,110,105,109,116,105,109,101,114,0,42,99,111,110,116,101,120,116,0,42,116,111,111,108,95,116,105,112,0,42,110,101,119,118,0,118,101,99,0,42,118,49,0,42,118,50,0,42,105,100,110,97,109,101,0,108,97,115,116,95,117,115,101,100,0,42,116,121,112,101,0,42,108,97,121,111,117,116,0,112,97,110,101,108,110,97,109,101,91,54,52,93,0,42,100,114,97,119,110,97,109,101,0,111,102,115,120,0,111,102,115,121,0,98,108,
111,99,107,115,105,122,101,120,0,98,108,111,99,107,115,105,122,101,121,0,108,97,98,101,108,111,102,115,0,115,111,114,116,111,114,100,101,114,0,42,97,99,116,105,118,101,100,97,116,97,0,108,97,121,111,117,116,95,112,97,110,101,108,95,115,116,97,116,101,115,0,108,97,121,111,117,116,95,112,97,110,101,108,95,115,116,97,116,101,115,95,99,108,111,99,107,0,108,105,115,116,95,105,100,91,49,50,56,93,0,108,97,121,111,117,116,95,116,121,112,
101,0,108,105,115,116,95,115,99,114,111,108,108,0,108,105,115,116,95,103,114,105,112,0,108,105,115,116,95,108,97,115,116,95,108,101,110,0,108,105,115,116,95,108,97,115,116,95,97,99,116,105,118,101,105,0,102,105,108,116,101,114,95,98,121,110,97,109,101,91,49,50,56,93,0,102,105,108,116,101,114,95,102,108,97,103,0,102,105,108,116,101,114,95,115,111,114,116,95,102,108,97,103,0,42,100,121,110,95,100,97,116,97,0,99,117,115,116,111,109,
95,104,101,105,103,104,116,0,115,99,114,111,108,108,95,111,102,102,115,101,116,0,109,97,116,91,51,93,91,51,93,0,112,114,101,118,105,101,119,95,105,100,91,54,52,93,0,105,100,95,115,101,115,115,105,111,110,95,117,105,100,0,99,117,114,95,102,105,120,101,100,95,104,101,105,103,104,116,0,115,105,122,101,95,109,105,110,0,115,105,122,101,95,109,97,120,0,97,108,105,103,110,0,42,116,111,111,108,0,105,115,95,116,111,111,108,95,115,101,116,
0,42,118,51,0,42,118,52,0,42,102,117,108,108,0,98,117,116,115,112,97,99,101,116,121,112,101,0,98,117,116,115,112,97,99,101,116,121,112,101,95,115,117,98,116,121,112,101,0,119,105,110,120,0,119,105,110,121,0,104,101,97,100,101,114,116,121,112,101,0,114,101,103,105,111,110,95,97,99,116,105,118,101,95,119,105,110,0,42,103,108,111,98,97,108,0,115,112,97,99,101,100,97,116,97,0,104,97,110,100,108,101,114,115,0,97,99,116,105,111,
110,122,111,110,101,115,0,119,105,110,114,99,116,0,99,97,116,101,103,111,114,121,95,115,99,114,111,108,108,0,114,101,103,105,111,110,116,121,112,101,0,97,108,105,103,110,109,101,110,116,0,111,118,101,114,108,97,112,0,102,108,97,103,102,117,108,108,115,99,114,101,101,110,0,112,97,110,101,108,115,0,112,97,110,101,108,115,95,99,97,116,101,103,111,114,121,95,97,99,116,105,118,101,0,117,105,95,108,105,115,116,115,0,117,105,95,112,114,101,118,
105,101,119,115,0,118,105,101,119,95,115,116,97,116,101,115,0,42,114,101,103,105,111,110,100,97,116,97,0,97,115,115,101,116,95,108,105,98,114,97,114,121,95,114,101,102,101,114,101,110,99,101,0,101,110,97,98,108,101,100,95,99,97,116,97,108,111,103,95,112,97,116,104,115,0,42,97,99,116,105,118,101,95,99,97,116,97,108,111,103,95,112,97,116,104,0,115,101,97,114,99,104,95,115,116,114,105,110,103,91,54,52,93,0,112,114,101,118,105,101,
119,95,115,105,122,101,0,100,105,115,112,108,97,121,95,102,108,97,103,0,112,114,101,102,101,114,114,101,100,95,114,111,119,95,99,111,117,110,116,0,105,110,115,116,97,110,99,101,95,102,108,97,103,0,115,104,101,108,118,101,115,0,42,97,99,116,105,118,101,95,115,104,101,108,102,0,111,114,105,103,95,119,105,100,116,104,0,111,114,105,103,95,104,101,105,103,104,116,0,111,114,105,103,95,102,112,115,0,116,111,112,0,98,111,116,116,111,109,0,108,
101,102,116,0,114,105,103,104,116,0,120,111,102,115,0,121,111,102,115,0,111,114,105,103,105,110,91,50,93,0,102,105,108,101,91,50,53,54,93,0,98,117,105,108,100,95,115,105,122,101,95,102,108,97,103,115,0,98,117,105,108,100,95,116,99,95,102,108,97,103,115,0,98,117,105,108,100,95,102,108,97,103,115,0,115,116,111,114,97,103,101,0,100,111,110,101,0,115,116,97,114,116,115,116,105,108,108,0,101,110,100,115,116,105,108,108,0,42,115,116,
114,105,112,100,97,116,97,0,42,99,114,111,112,0,42,116,114,97,110,115,102,111,114,109,0,42,99,111,108,111,114,95,98,97,108,97,110,99,101,0,115,116,114,105,112,95,102,114,97,109,101,95,105,110,100,101,120,0,114,101,116,105,109,105,110,103,95,102,97,99,116,111,114,0,111,114,105,103,105,110,97,108,95,115,116,114,105,112,95,102,114,97,109,101,95,105,110,100,101,120,0,111,114,105,103,105,110,97,108,95,114,101,116,105,109,105,110,103,95,102,
97,99,116,111,114,0,115,116,97,114,116,111,102,115,0,101,110,100,111,102,115,0,109,97,99,104,105,110,101,0,115,116,97,114,116,100,105,115,112,0,101,110,100,100,105,115,112,0,109,117,108,0,115,116,114,101,97,109,105,110,100,101,120,0,109,117,108,116,105,99,97,109,95,115,111,117,114,99,101,0,99,108,105,112,95,102,108,97,103,0,42,115,116,114,105,112,0,42,115,99,101,110,101,95,99,97,109,101,114,97,0,42,109,97,115,107,0,101,102,102,
101,99,116,95,102,97,100,101,114,0,115,112,101,101,100,95,102,97,100,101,114,0,42,115,101,113,49,0,42,115,101,113,50,0,115,101,113,98,97,115,101,0,99,111,110,110,101,99,116,105,111,110,115,0,42,115,111,117,110,100,0,42,115,99,101,110,101,95,115,111,117,110,100,0,112,97,110,0,115,116,114,111,98,101,0,115,111,117,110,100,95,111,102,102,115,101,116,0,42,101,102,102,101,99,116,100,97,116,97,0,97,110,105,109,95,115,116,97,114,116,
111,102,115,0,97,110,105,109,95,101,110,100,111,102,115,0,98,108,101,110,100,95,111,112,97,99,105,116,121,0,95,112,97,100,57,0,109,101,100,105,97,95,112,108,97,121,98,97,99,107,95,114,97,116,101,0,115,112,101,101,100,95,102,97,99,116,111,114,0,42,114,101,116,105,109,105,110,103,95,104,97,110,100,108,101,115,0,114,101,116,105,109,105,110,103,95,104,97,110,100,108,101,95,110,117,109,0,42,111,108,100,98,97,115,101,112,0,42,111,108,
100,95,99,104,97,110,110,101,108,115,0,42,112,97,114,115,101,113,0,100,105,115,112,95,114,97,110,103,101,91,50,93,0,42,115,101,113,95,114,101,102,0,42,115,116,114,105,112,95,108,111,111,107,117,112,0,42,109,101,100,105,97,95,112,114,101,115,101,110,99,101,0,42,116,104,117,109,98,110,97,105,108,95,99,97,99,104,101,0,42,105,110,116,114,97,95,102,114,97,109,101,95,99,97,99,104,101,0,42,115,111,117,114,99,101,95,105,109,97,103,
101,95,99,97,99,104,101,0,42,102,105,110,97,108,95,105,109,97,103,101,95,99,97,99,104,101,0,42,115,101,113,98,97,115,101,112,0,42,100,105,115,112,108,97,121,101,100,95,99,104,97,110,110,101,108,115,0,109,101,116,97,115,116,97,99,107,0,42,97,99,116,95,115,101,113,0,97,99,116,95,105,109,97,103,101,100,105,114,91,49,48,50,52,93,0,97,99,116,95,115,111,117,110,100,100,105,114,91,49,48,50,52,93,0,112,114,111,120,121,95,
100,105,114,91,49,48,50,52,93,0,112,114,111,120,121,95,115,116,111,114,97,103,101,0,111,118,101,114,95,111,102,115,0,111,118,101,114,95,99,102,114,97,0,111,118,101,114,95,102,108,97,103,0,111,118,101,114,95,98,111,114,100,101,114,0,115,104,111,119,95,109,105,115,115,105,110,103,95,109,101,100,105,97,95,102,108,97,103,0,42,112,114,101,102,101,116,99,104,95,106,111,98,0,101,100,103,101,87,105,100,116,104,0,102,111,114,119,97,114,100,
0,119,105,112,101,116,121,112,101,0,102,77,105,110,105,0,102,67,108,97,109,112,0,102,66,111,111,115,116,0,100,68,105,115,116,0,100,81,117,97,108,105,116,121,0,98,78,111,67,111,109,112,0,83,99,97,108,101,120,73,110,105,0,83,99,97,108,101,121,73,110,105,0,120,73,110,105,0,121,73,110,105,0,114,111,116,73,110,105,0,117,110,105,102,111,114,109,95,115,99,97,108,101,0,99,111,108,91,51,93,0,42,102,114,97,109,101,77,97,112,
0,103,108,111,98,97,108,83,112,101,101,100,0,115,112,101,101,100,95,99,111,110,116,114,111,108,95,116,121,112,101,0,115,112,101,101,100,95,102,97,100,101,114,95,108,101,110,103,116,104,0,115,112,101,101,100,95,102,97,100,101,114,95,102,114,97,109,101,95,110,117,109,98,101,114,0,116,101,120,116,91,53,49,50,93,0,42,116,101,120,116,95,102,111,110,116,0,116,101,120,116,95,98,108,102,95,105,100,0,116,101,120,116,95,115,105,122,101,0,115,
104,97,100,111,119,95,99,111,108,111,114,91,52,93,0,98,111,120,95,99,111,108,111,114,91,52,93,0,111,117,116,108,105,110,101,95,99,111,108,111,114,91,52,93,0,108,111,99,91,50,93,0,119,114,97,112,95,119,105,100,116,104,0,98,111,120,95,109,97,114,103,105,110,0,98,111,120,95,114,111,117,110,100,110,101,115,115,0,115,104,97,100,111,119,95,97,110,103,108,101,0,115,104,97,100,111,119,95,111,102,102,115,101,116,0,115,104,97,100,111,
119,95,98,108,117,114,0,111,117,116,108,105,110,101,95,119,105,100,116,104,0,99,117,114,115,111,114,95,111,102,102,115,101,116,0,115,101,108,101,99,116,105,111,110,95,115,116,97,114,116,95,111,102,102,115,101,116,0,115,101,108,101,99,116,105,111,110,95,101,110,100,95,111,102,102,115,101,116,0,97,110,99,104,111,114,95,120,0,97,110,99,104,111,114,95,121,0,98,108,101,110,100,95,101,102,102,101,99,116,0,109,97,115,107,95,105,110,112,117,116,
95,116,121,112,101,0,109,97,115,107,95,116,105,109,101,0,42,109,97,115,107,95,115,101,113,117,101,110,99,101,0,42,109,97,115,107,95,105,100,0,99,111,108,111,114,95,98,97,108,97,110,99,101,0,99,111,108,111,114,95,109,117,108,116,105,112,108,121,0,99,117,114,118,101,95,109,97,112,112,105,110,103,0,98,114,105,103,104,116,0,119,104,105,116,101,95,118,97,108,117,101,91,51,93,0,97,100,97,112,116,97,116,105,111,110,0,99,111,114,114,
101,99,116,105,111,110,0,103,114,97,112,104,105,99,115,0,117,105,100,95,0,42,102,120,95,115,104,0,42,102,120,95,115,104,95,98,0,42,102,120,95,115,104,95,99,0,115,104,97,100,101,114,102,120,0,114,97,100,105,117,115,91,50,93,0,108,111,119,95,99,111,108,111,114,91,52,93,0,104,105,103,104,95,99,111,108,111,114,91,52,93,0,102,108,105,112,109,111,100,101,0,103,108,111,119,95,99,111,108,111,114,91,52,93,0,115,101,108,101,99,
116,95,99,111,108,111,114,91,51,93,0,98,108,117,114,91,50,93,0,114,103,98,97,91,52,93,0,114,105,109,95,114,103,98,91,51,93,0,109,97,115,107,95,114,103,98,91,51,93,0,115,104,97,100,111,119,95,114,103,98,97,91,52,93,0,116,114,97,110,115,112,97,114,101,110,116,0,42,110,101,119,112,97,99,107,101,100,102,105,108,101,0,97,116,116,101,110,117,97,116,105,111,110,0,109,105,110,95,103,97,105,110,0,109,97,120,95,103,97,105,
110,0,111,102,102,115,101,116,95,116,105,109,101,0,42,119,97,118,101,102,111,114,109,0,42,115,112,105,110,108,111,99,107,0,115,97,109,112,108,101,114,97,116,101,0,114,112,116,95,109,97,115,107,0,115,112,97,99,101,95,115,117,98,116,121,112,101,0,109,97,105,110,98,0,109,97,105,110,98,111,0,109,97,105,110,98,117,115,101,114,0,118,105,115,105,98,108,101,95,116,97,98,115,0,111,117,116,108,105,110,101,114,95,115,121,110,99,0,100,97,
116,97,105,99,111,110,0,42,112,105,110,105,100,0,42,116,101,120,117,115,101,114,0,116,114,101,101,0,42,116,114,101,101,115,116,111,114,101,0,111,117,116,108,105,110,101,118,105,115,0,108,105,98,95,111,118,101,114,114,105,100,101,95,118,105,101,119,95,109,111,100,101,0,115,116,111,114,101,102,108,97,103,0,115,101,97,114,99,104,95,102,108,97,103,115,0,115,121,110,99,95,115,101,108,101,99,116,95,100,105,114,116,121,0,102,105,108,116,101,114,
95,115,116,97,116,101,0,115,104,111,119,95,114,101,115,116,114,105,99,116,95,102,108,97,103,115,0,102,105,108,116,101,114,95,105,100,95,116,121,112,101,0,103,104,111,115,116,95,99,117,114,118,101,115,0,42,97,100,115,0,99,117,114,115,111,114,84,105,109,101,0,99,117,114,115,111,114,86,97,108,0,97,114,111,117,110,100,0,99,104,97,110,115,104,111,119,110,0,122,101,98,114,97,0,111,118,101,114,108,97,121,95,116,121,112,101,0,100,114,97,
119,95,102,108,97,103,0,103,105,122,109,111,95,102,108,97,103,0,99,117,114,115,111,114,91,50,93,0,112,114,101,118,105,101,119,95,111,118,101,114,108,97,121,0,116,105,109,101,108,105,110,101,95,111,118,101,114,108,97,121,0,99,97,99,104,101,95,111,118,101,114,108,97,121,0,100,114,97,119,95,116,121,112,101,0,111,118,101,114,108,97,121,95,109,111,100,101,0,98,108,101,110,100,95,102,97,99,116,111,114,0,116,105,116,108,101,91,57,54,93,
0,100,105,114,91,49,48,57,48,93,0,114,101,110,97,109,101,102,105,108,101,91,50,53,54,93,0,114,101,110,97,109,101,95,102,108,97,103,0,42,114,101,110,97,109,101,95,105,100,0,102,105,108,116,101,114,95,103,108,111,98,91,50,53,54,93,0,102,105,108,116,101,114,95,115,101,97,114,99,104,91,54,52,93,0,102,105,108,116,101,114,95,105,100,0,97,99,116,105,118,101,95,102,105,108,101,0,104,105,103,104,108,105,103,104,116,95,102,105,108,
101,0,115,101,108,95,102,105,114,115,116,0,115,101,108,95,108,97,115,116,0,116,104,117,109,98,110,97,105,108,95,115,105,122,101,0,108,105,115,116,95,116,104,117,109,98,110,97,105,108,95,115,105,122,101,0,108,105,115,116,95,99,111,108,117,109,110,95,115,105,122,101,0,115,111,114,116,0,100,101,116,97,105,108,115,95,102,108,97,103,115,0,114,101,99,117,114,115,105,111,110,95,108,101,118,101,108,0,98,97,115,101,95,112,97,114,97,109,115,0,
97,115,115,101,116,95,108,105,98,114,97,114,121,95,114,101,102,0,97,115,115,101,116,95,99,97,116,97,108,111,103,95,118,105,115,105,98,105,108,105,116,121,0,105,109,112,111,114,116,95,116,121,112,101,0,105,109,112,111,114,116,95,102,108,97,103,115,0,98,114,111,119,115,101,95,109,111,100,101,0,102,111,108,100,101,114,115,95,112,114,101,118,0,102,111,108,100,101,114,115,95,110,101,120,116,0,42,112,97,114,97,109,115,0,42,97,115,115,101,116,
95,112,97,114,97,109,115,0,42,102,105,108,101,115,0,42,102,111,108,100,101,114,115,95,112,114,101,118,0,42,102,111,108,100,101,114,115,95,110,101,120,116,0,102,111,108,100,101,114,95,104,105,115,116,111,114,105,101,115,0,42,111,112,0,42,115,109,111,111,116,104,115,99,114,111,108,108,95,116,105,109,101,114,0,42,112,114,101,118,105,101,119,115,95,116,105,109,101,114,0,114,101,99,101,110,116,110,114,0,98,111,111,107,109,97,114,107,110,114,0,
115,121,115,116,101,109,110,114,0,115,121,115,116,101,109,95,98,111,111,107,109,97,114,107,110,114,0,112,97,115,115,101,112,97,114,116,111,117,116,95,97,108,112,104,97,0,42,105,109,97,103,101,0,115,99,111,112,101,115,0,115,97,109,112,108,101,95,108,105,110,101,95,104,105,115,116,0,99,101,110,116,120,0,99,101,110,116,121,0,112,105,110,0,112,105,120,101,108,95,115,110,97,112,95,109,111,100,101,0,108,111,99,107,0,100,116,95,117,118,0,
100,116,95,117,118,115,116,114,101,116,99,104,0,103,114,105,100,95,115,104,97,112,101,95,115,111,117,114,99,101,0,117,118,95,111,112,97,99,105,116,121,0,117,118,95,102,97,99,101,95,111,112,97,99,105,116,121,0,115,116,114,101,116,99,104,95,111,112,97,99,105,116,121,0,116,105,108,101,95,103,114,105,100,95,115,104,97,112,101,91,50,93,0,99,117,115,116,111,109,95,103,114,105,100,95,115,117,98,100,105,118,91,50,93,0,109,97,115,107,95,
105,110,102,111,0,111,118,101,114,108,97,121,0,42,116,101,120,116,0,108,104,101,105,103,104,116,0,116,97,98,110,117,109,98,101,114,0,119,111,114,100,119,114,97,112,0,100,111,112,108,117,103,105,110,115,0,115,104,111,119,108,105,110,101,110,114,115,0,115,104,111,119,115,121,110,116,97,120,0,108,105,110,101,95,104,108,105,103,104,116,0,111,118,101,114,119,114,105,116,101,0,108,105,118,101,95,101,100,105,116,0,95,112,97,100,50,91,49,93,0,
102,105,110,100,115,116,114,91,50,53,54,93,0,114,101,112,108,97,99,101,115,116,114,91,50,53,54,93,0,109,97,114,103,105,110,95,99,111,108,117,109,110,0,42,112,121,95,100,114,97,119,0,42,112,121,95,101,118,101,110,116,0,42,112,121,95,98,117,116,116,111,110,0,42,112,121,95,98,114,111,119,115,101,114,99,97,108,108,98,97,99,107,0,42,112,121,95,103,108,111,98,97,108,100,105,99,116,0,108,97,115,116,115,112,97,99,101,0,115,99,
114,105,112,116,110,97,109,101,91,49,48,50,52,93,0,115,99,114,105,112,116,97,114,103,91,50,53,54,93,0,109,101,110,117,110,114,0,42,98,117,116,95,114,101,102,115,0,112,97,114,101,110,116,95,107,101,121,0,110,111,100,101,95,110,97,109,101,91,54,52,93,0,100,105,115,112,108,97,121,95,110,97,109,101,91,54,52,93,0,112,114,101,118,105,101,119,95,115,104,97,112,101,0,105,110,115,101,114,116,95,111,102,115,95,100,105,114,0,116,114,
101,101,112,97,116,104,0,42,101,100,105,116,116,114,101,101,0,116,114,101,101,95,105,100,110,97,109,101,91,54,52,93,0,116,114,101,101,116,121,112,101,0,116,101,120,102,114,111,109,0,115,104,97,100,101,114,102,114,111,109,0,103,101,111,109,101,116,114,121,95,110,111,100,101,115,95,116,121,112,101,0,42,103,101,111,109,101,116,114,121,95,110,111,100,101,115,95,116,111,111,108,95,116,114,101,101,0,108,101,110,95,97,108,108,111,99,0,42,108,105,
110,101,0,115,99,114,111,108,108,98,97,99,107,0,104,105,115,116,111,114,121,0,112,114,111,109,112,116,91,50,53,54,93,0,108,97,110,103,117,97,103,101,91,51,50,93,0,104,105,115,116,111,114,121,95,105,110,100,101,120,0,115,101,108,95,115,116,97,114,116,0,115,101,108,95,101,110,100,0,102,105,108,116,101,114,95,116,121,112,101,0,102,105,108,116,101,114,91,54,52,93,0,120,108,111,99,107,111,102,0,121,108,111,99,107,111,102,0,112,97,
116,104,95,108,101,110,103,116,104,0,115,116,97,98,109,97,116,91,52,93,91,52,93,0,117,110,105,115,116,97,98,109,97,116,91,52,93,91,52,93,0,112,111,115,116,112,114,111,99,95,102,108,97,103,0,103,112,101,110,99,105,108,95,115,114,99,0,42,100,105,115,112,108,97,121,95,110,97,109,101,0,114,101,102,101,114,101,110,99,101,95,105,110,100,101,120,0,118,105,101,119,101,114,95,112,97,116,104,0,42,105,110,115,116,97,110,99,101,95,105,
100,115,0,105,110,115,116,97,110,99,101,95,105,100,115,95,110,117,109,0,103,101,111,109,101,116,114,121,95,99,111,109,112,111,110,101,110,116,95,116,121,112,101,0,111,98,106,101,99,116,95,101,118,97,108,95,115,116,97,116,101,0,95,112,97,100,49,91,53,93,0,108,97,121,101,114,95,105,110,100,101,120,0,42,42,99,111,108,117,109,110,115,0,110,117,109,95,99,111,108,117,109,110,115,0,99,111,108,117,109,110,95,117,115,101,95,99,108,111,99,
107,0,42,42,116,97,98,108,101,115,0,110,117,109,95,116,97,98,108,101,115,0,114,111,119,95,102,105,108,116,101,114,115,0,103,101,111,109,101,116,114,121,95,105,100,0,116,97,98,108,101,95,117,115,101,95,99,108,111,99,107,0,97,99,116,105,118,101,95,118,105,101,119,101,114,95,112,97,116,104,95,105,110,100,101,120,0,99,111,108,117,109,110,95,110,97,109,101,91,54,52,93,0,118,97,108,117,101,95,105,110,116,0,118,97,108,117,101,95,105,
110,116,50,91,50,93,0,42,118,97,108,117,101,95,115,116,114,105,110,103,0,118,97,108,117,101,95,102,108,111,97,116,0,118,97,108,117,101,95,102,108,111,97,116,50,91,50,93,0,118,97,108,117,101,95,102,108,111,97,116,51,91,51,93,0,118,97,108,117,101,95,99,111,108,111,114,91,52,93,0,118,111,108,117,109,101,95,109,97,120,0,118,111,108,117,109,101,95,109,105,110,0,100,105,115,116,97,110,99,101,95,109,97,120,0,100,105,115,116,97,
110,99,101,95,114,101,102,101,114,101,110,99,101,0,99,111,110,101,95,97,110,103,108,101,95,111,117,116,101,114,0,99,111,110,101,95,97,110,103,108,101,95,105,110,110,101,114,0,99,111,110,101,95,118,111,108,117,109,101,95,111,117,116,101,114,0,42,102,111,114,109,97,116,0,42,99,111,109,112,105,108,101,100,0,42,99,117,114,108,0,42,115,101,108,108,0,99,117,114,99,0,115,101,108,99,0,109,116,105,109,101,0,116,101,120,99,111,0,109,97,
112,116,111,0,98,108,101,110,100,116,121,112,101,0,112,114,111,106,120,0,112,114,111,106,121,0,112,114,111,106,122,0,98,114,117,115,104,95,109,97,112,95,109,111,100,101,0,98,114,117,115,104,95,97,110,103,108,101,95,109,111,100,101,0,119,104,105,99,104,95,111,117,116,112,117,116,0,111,102,115,91,51,93,0,114,111,116,0,114,97,110,100,111,109,95,97,110,103,108,101,0,107,0,100,101,102,95,118,97,114,0,99,111,108,102,97,99,0,97,108,
112,104,97,102,97,99,0,116,105,109,101,102,97,99,0,108,101,110,103,116,104,102,97,99,0,107,105,110,107,102,97,99,0,107,105,110,107,97,109,112,102,97,99,0,114,111,117,103,104,102,97,99,0,112,97,100,101,110,115,102,97,99,0,103,114,97,118,105,116,121,102,97,99,0,108,105,102,101,102,97,99,0,115,105,122,101,102,97,99,0,105,118,101,108,102,97,99,0,102,105,101,108,100,102,97,99,0,116,119,105,115,116,102,97,99,0,102,97,108,108,
111,102,102,95,115,111,102,116,110,101,115,115,0,112,115,121,115,95,99,97,99,104,101,95,115,112,97,99,101,0,111,98,95,99,97,99,104,101,95,115,112,97,99,101,0,42,112,111,105,110,116,95,116,114,101,101,0,42,112,111,105,110,116,95,100,97,116,97,0,110,111,105,115,101,95,115,105,122,101,0,110,111,105,115,101,95,100,101,112,116,104,0,110,111,105,115,101,95,105,110,102,108,117,101,110,99,101,0,110,111,105,115,101,95,98,97,115,105,115,0,
110,111,105,115,101,95,102,97,99,0,115,112,101,101,100,95,115,99,97,108,101,0,102,97,108,108,111,102,102,95,115,112,101,101,100,95,115,99,97,108,101,0,42,102,97,108,108,111,102,102,95,99,117,114,118,101,0,114,102,97,99,0,103,102,97,99,0,98,102,97,99,0,102,105,108,116,101,114,115,105,122,101,0,109,103,95,72,0,109,103,95,108,97,99,117,110,97,114,105,116,121,0,109,103,95,111,99,116,97,118,101,115,0,109,103,95,111,102,102,115,
101,116,0,109,103,95,103,97,105,110,0,100,105,115,116,95,97,109,111,117,110,116,0,110,115,95,111,117,116,115,99,97,108,101,0,118,110,95,119,49,0,118,110,95,119,50,0,118,110,95,119,51,0,118,110,95,119,52,0,118,110,95,109,101,120,112,0,118,110,95,100,105,115,116,109,0,118,110,95,99,111,108,116,121,112,101,0,110,111,105,115,101,100,101,112,116,104,0,110,111,105,115,101,116,121,112,101,0,110,111,105,115,101,98,97,115,105,115,0,110,
111,105,115,101,98,97,115,105,115,50,0,105,109,97,102,108,97,103,0,99,114,111,112,120,109,105,110,0,99,114,111,112,121,109,105,110,0,99,114,111,112,120,109,97,120,0,99,114,111,112,121,109,97,120,0,116,101,120,102,105,108,116,101,114,0,97,102,109,97,120,0,120,114,101,112,101,97,116,0,121,114,101,112,101,97,116,0,99,104,101,99,107,101,114,100,105,115,116,0,109,105,110,91,51,93,0,109,97,120,91,51,93,0,99,111,98,97,0,98,108,
101,110,100,95,99,111,108,111,114,91,51,93,0,117,105,102,111,110,116,95,105,100,0,112,111,105,110,116,115,0,105,116,97,108,105,99,0,98,111,108,100,0,115,104,97,100,111,119,0,115,104,97,100,120,0,115,104,97,100,121,0,115,104,97,100,111,119,97,108,112,104,97,0,115,104,97,100,111,119,99,111,108,111,114,0,99,104,97,114,97,99,116,101,114,95,119,101,105,103,104,116,0,112,97,110,101,108,116,105,116,108,101,0,103,114,111,117,112,108,97,
98,101,108,0,119,105,100,103,101,116,0,116,111,111,108,116,105,112,0,112,97,110,101,108,122,111,111,109,0,109,105,110,108,97,98,101,108,99,104,97,114,115,0,109,105,110,119,105,100,103,101,116,99,104,97,114,115,0,99,111,108,117,109,110,115,112,97,99,101,0,116,101,109,112,108,97,116,101,115,112,97,99,101,0,98,111,120,115,112,97,99,101,0,98,117,116,116,111,110,115,112,97,99,101,120,0,98,117,116,116,111,110,115,112,97,99,101,121,0,112,
97,110,101,108,115,112,97,99,101,0,112,97,110,101,108,111,117,116,101,114,0,111,117,116,108,105,110,101,91,52,93,0,105,110,110,101,114,91,52,93,0,105,110,110,101,114,95,115,101,108,91,52,93,0,105,116,101,109,91,52,93,0,116,101,120,116,91,52,93,0,116,101,120,116,95,115,101,108,91,52,93,0,115,104,97,100,101,100,0,115,104,97,100,101,116,111,112,0,115,104,97,100,101,100,111,119,110,0,114,111,117,110,100,110,101,115,115,0,101,114,
114,111,114,91,52,93,0,119,97,114,110,105,110,103,91,52,93,0,105,110,102,111,91,52,93,0,115,117,99,99,101,115,115,91,52,93,0,105,110,110,101,114,95,97,110,105,109,91,52,93,0,105,110,110,101,114,95,97,110,105,109,95,115,101,108,91,52,93,0,105,110,110,101,114,95,107,101,121,91,52,93,0,105,110,110,101,114,95,107,101,121,95,115,101,108,91,52,93,0,105,110,110,101,114,95,100,114,105,118,101,110,91,52,93,0,105,110,110,101,114,
95,100,114,105,118,101,110,95,115,101,108,91,52,93,0,105,110,110,101,114,95,111,118,101,114,114,105,100,100,101,110,91,52,93,0,105,110,110,101,114,95,111,118,101,114,114,105,100,100,101,110,95,115,101,108,91,52,93,0,105,110,110,101,114,95,99,104,97,110,103,101,100,91,52,93,0,105,110,110,101,114,95,99,104,97,110,103,101,100,95,115,101,108,91,52,93,0,104,101,97,100,101,114,91,52,93,0,98,97,99,107,91,52,93,0,115,117,98,95,98,
97,99,107,91,52,93,0,119,99,111,108,95,114,101,103,117,108,97,114,0,119,99,111,108,95,116,111,111,108,0,119,99,111,108,95,116,111,111,108,98,97,114,95,105,116,101,109,0,119,99,111,108,95,116,101,120,116,0,119,99,111,108,95,114,97,100,105,111,0,119,99,111,108,95,111,112,116,105,111,110,0,119,99,111,108,95,116,111,103,103,108,101,0,119,99,111,108,95,110,117,109,0,119,99,111,108,95,110,117,109,115,108,105,100,101,114,0,119,99,111,
108,95,116,97,98,0,119,99,111,108,95,109,101,110,117,0,119,99,111,108,95,112,117,108,108,100,111,119,110,0,119,99,111,108,95,109,101,110,117,95,98,97,99,107,0,119,99,111,108,95,109,101,110,117,95,105,116,101,109,0,119,99,111,108,95,116,111,111,108,116,105,112,0,119,99,111,108,95,98,111,120,0,119,99,111,108,95,115,99,114,111,108,108,0,119,99,111,108,95,112,114,111,103,114,101,115,115,0,119,99,111,108,95,108,105,115,116,95,105,116,
101,109,0,119,99,111,108,95,112,105,101,95,109,101,110,117,0,119,99,111,108,95,115,116,97,116,101,0,119,105,100,103,101,116,95,101,109,98,111,115,115,91,52,93,0,109,101,110,117,95,115,104,97,100,111,119,95,102,97,99,0,109,101,110,117,95,115,104,97,100,111,119,95,119,105,100,116,104,0,101,100,105,116,111,114,95,98,111,114,100,101,114,91,52,93,0,101,100,105,116,111,114,95,111,117,116,108,105,110,101,91,52,93,0,101,100,105,116,111,114,
95,111,117,116,108,105,110,101,95,97,99,116,105,118,101,91,52,93,0,116,114,97,110,115,112,97,114,101,110,116,95,99,104,101,99,107,101,114,95,112,114,105,109,97,114,121,91,52,93,0,116,114,97,110,115,112,97,114,101,110,116,95,99,104,101,99,107,101,114,95,115,101,99,111,110,100,97,114,121,91,52,93,0,116,114,97,110,115,112,97,114,101,110,116,95,99,104,101,99,107,101,114,95,115,105,122,101,0,105,99,111,110,95,97,108,112,104,97,0,105,
99,111,110,95,115,97,116,117,114,97,116,105,111,110,0,119,105,100,103,101,116,95,116,101,120,116,95,99,117,114,115,111,114,91,52,93,0,120,97,120,105,115,91,52,93,0,121,97,120,105,115,91,52,93,0,122,97,120,105,115,91,52,93,0,103,105,122,109,111,95,104,105,91,52,93,0,103,105,122,109,111,95,112,114,105,109,97,114,121,91,52,93,0,103,105,122,109,111,95,115,101,99,111,110,100,97,114,121,91,52,93,0,103,105,122,109,111,95,118,105,
101,119,95,97,108,105,103,110,91,52,93,0,103,105,122,109,111,95,97,91,52,93,0,103,105,122,109,111,95,98,91,52,93,0,105,99,111,110,95,115,99,101,110,101,91,52,93,0,105,99,111,110,95,99,111,108,108,101,99,116,105,111,110,91,52,93,0,105,99,111,110,95,111,98,106,101,99,116,91,52,93,0,105,99,111,110,95,111,98,106,101,99,116,95,100,97,116,97,91,52,93,0,105,99,111,110,95,109,111,100,105,102,105,101,114,91,52,93,0,105,
99,111,110,95,115,104,97,100,105,110,103,91,52,93,0,105,99,111,110,95,102,111,108,100,101,114,91,52,93,0,105,99,111,110,95,97,117,116,111,107,101,121,91,52,93,0,105,99,111,110,95,98,111,114,100,101,114,95,105,110,116,101,110,115,105,116,121,0,112,97,110,101,108,95,114,111,117,110,100,110,101,115,115,0,104,101,97,100,101,114,95,98,97,99,107,91,52,93,0,98,97,99,107,95,103,114,97,100,91,52,93,0,115,104,111,119,95,98,97,99,
107,95,103,114,97,100,0,116,105,116,108,101,91,52,93,0,116,101,120,116,95,104,105,91,52,93,0,104,101,97,100,101,114,95,116,105,116,108,101,91,52,93,0,104,101,97,100,101,114,95,116,101,120,116,91,52,93,0,104,101,97,100,101,114,95,116,101,120,116,95,104,105,91,52,93,0,116,97,98,95,97,99,116,105,118,101,91,52,93,0,116,97,98,95,105,110,97,99,116,105,118,101,91,52,93,0,116,97,98,95,98,97,99,107,91,52,93,0,116,97,
98,95,111,117,116,108,105,110,101,91,52,93,0,98,117,116,116,111,110,91,52,93,0,98,117,116,116,111,110,95,116,105,116,108,101,91,52,93,0,98,117,116,116,111,110,95,116,101,120,116,91,52,93,0,98,117,116,116,111,110,95,116,101,120,116,95,104,105,91,52,93,0,108,105,115,116,91,52,93,0,108,105,115,116,95,116,105,116,108,101,91,52,93,0,108,105,115,116,95,116,101,120,116,91,52,93,0,108,105,115,116,95,116,101,120,116,95,104,105,91,
52,93,0,110,97,118,105,103,97,116,105,111,110,95,98,97,114,91,52,93,0,101,120,101,99,117,116,105,111,110,95,98,117,116,115,91,52,93,0,112,97,110,101,108,99,111,108,111,114,115,0,97,115,115,101,116,95,115,104,101,108,102,0,115,104,97,100,101,49,91,52,93,0,115,104,97,100,101,50,91,52,93,0,104,105,108,105,116,101,91,52,93,0,103,114,105,100,91,52,93,0,118,105,101,119,95,111,118,101,114,108,97,121,91,52,93,0,119,105,114,
101,91,52,93,0,119,105,114,101,95,101,100,105,116,91,52,93,0,115,101,108,101,99,116,91,52,93,0,108,97,109,112,91,52,93,0,115,112,101,97,107,101,114,91,52,93,0,101,109,112,116,121,91,52,93,0,99,97,109,101,114,97,91,52,93,0,97,99,116,105,118,101,91,52,93,0,103,114,111,117,112,91,52,93,0,103,114,111,117,112,95,97,99,116,105,118,101,91,52,93,0,116,114,97,110,115,102,111,114,109,91,52,93,0,118,101,114,116,101,120,
91,52,93,0,118,101,114,116,101,120,95,115,101,108,101,99,116,91,52,93,0,118,101,114,116,101,120,95,97,99,116,105,118,101,91,52,93,0,118,101,114,116,101,120,95,98,101,118,101,108,91,52,93,0,118,101,114,116,101,120,95,117,110,114,101,102,101,114,101,110,99,101,100,91,52,93,0,101,100,103,101,91,52,93,0,101,100,103,101,95,115,101,108,101,99,116,91,52,93,0,101,100,103,101,95,109,111,100,101,95,115,101,108,101,99,116,91,52,93,0,
101,100,103,101,95,115,101,97,109,91,52,93,0,101,100,103,101,95,115,104,97,114,112,91,52,93,0,101,100,103,101,95,102,97,99,101,115,101,108,91,52,93,0,101,100,103,101,95,99,114,101,97,115,101,91,52,93,0,101,100,103,101,95,98,101,118,101,108,91,52,93,0,102,97,99,101,91,52,93,0,102,97,99,101,95,115,101,108,101,99,116,91,52,93,0,102,97,99,101,95,109,111,100,101,95,115,101,108,101,99,116,91,52,93,0,102,97,99,101,95,
114,101,116,111,112,111,108,111,103,121,91,52,93,0,102,97,99,101,95,98,97,99,107,91,52,93,0,102,97,99,101,95,102,114,111,110,116,91,52,93,0,102,97,99,101,95,100,111,116,91,52,93,0,101,120,116,114,97,95,101,100,103,101,95,108,101,110,91,52,93,0,101,120,116,114,97,95,101,100,103,101,95,97,110,103,108,101,91,52,93,0,101,120,116,114,97,95,102,97,99,101,95,97,110,103,108,101,91,52,93,0,101,120,116,114,97,95,102,97,99,
101,95,97,114,101,97,91,52,93,0,110,111,114,109,97,108,91,52,93,0,118,101,114,116,101,120,95,110,111,114,109,97,108,91,52,93,0,108,111,111,112,95,110,111,114,109,97,108,91,52,93,0,98,111,110,101,95,115,111,108,105,100,91,52,93,0,98,111,110,101,95,112,111,115,101,91,52,93,0,98,111,110,101,95,112,111,115,101,95,97,99,116,105,118,101,91,52,93,0,98,111,110,101,95,108,111,99,107,101,100,95,119,101,105,103,104,116,91,52,93,
0,115,116,114,105,112,91,52,93,0,115,116,114,105,112,95,115,101,108,101,99,116,91,52,93,0,99,102,114,97,109,101,91,52,93,0,98,101,102,111,114,101,95,99,117,114,114,101,110,116,95,102,114,97,109,101,91,52,93,0,97,102,116,101,114,95,99,117,114,114,101,110,116,95,102,114,97,109,101,91,52,93,0,116,105,109,101,95,107,101,121,102,114,97,109,101,91,52,93,0,116,105,109,101,95,103,112,95,107,101,121,102,114,97,109,101,91,52,93,0,
102,114,101,101,115,116,121,108,101,95,101,100,103,101,95,109,97,114,107,91,52,93,0,102,114,101,101,115,116,121,108,101,95,102,97,99,101,95,109,97,114,107,91,52,93,0,115,99,114,117,98,98,105,110,103,95,98,97,99,107,103,114,111,117,110,100,91,52,93,0,116,105,109,101,95,109,97,114,107,101,114,95,108,105,110,101,91,52,93,0,116,105,109,101,95,109,97,114,107,101,114,95,108,105,110,101,95,115,101,108,101,99,116,101,100,91,52,93,0,110,
117,114,98,95,117,108,105,110,101,91,52,93,0,110,117,114,98,95,118,108,105,110,101,91,52,93,0,97,99,116,95,115,112,108,105,110,101,91,52,93,0,110,117,114,98,95,115,101,108,95,117,108,105,110,101,91,52,93,0,110,117,114,98,95,115,101,108,95,118,108,105,110,101,91,52,93,0,108,97,115,116,115,101,108,95,112,111,105,110,116,91,52,93,0,104,97,110,100,108,101,95,102,114,101,101,91,52,93,0,104,97,110,100,108,101,95,97,117,116,111,
91,52,93,0,104,97,110,100,108,101,95,118,101,99,116,91,52,93,0,104,97,110,100,108,101,95,97,108,105,103,110,91,52,93,0,104,97,110,100,108,101,95,97,117,116,111,95,99,108,97,109,112,101,100,91,52,93,0,104,97,110,100,108,101,95,115,101,108,95,102,114,101,101,91,52,93,0,104,97,110,100,108,101,95,115,101,108,95,97,117,116,111,91,52,93,0,104,97,110,100,108,101,95,115,101,108,95,118,101,99,116,91,52,93,0,104,97,110,100,108,
101,95,115,101,108,95,97,108,105,103,110,91,52,93,0,104,97,110,100,108,101,95,115,101,108,95,97,117,116,111,95,99,108,97,109,112,101,100,91,52,93,0,100,115,95,99,104,97,110,110,101,108,91,52,93,0,100,115,95,115,117,98,99,104,97,110,110,101,108,91,52,93,0,100,115,95,105,112,111,108,105,110,101,91,52,93,0,107,101,121,116,121,112,101,95,107,101,121,102,114,97,109,101,91,52,93,0,107,101,121,116,121,112,101,95,101,120,116,114,101,
109,101,91,52,93,0,107,101,121,116,121,112,101,95,98,114,101,97,107,100,111,119,110,91,52,93,0,107,101,121,116,121,112,101,95,106,105,116,116,101,114,91,52,93,0,107,101,121,116,121,112,101,95,109,111,118,101,104,111,108,100,91,52,93,0,107,101,121,116,121,112,101,95,103,101,110,101,114,97,116,101,100,91,52,93,0,107,101,121,116,121,112,101,95,107,101,121,102,114,97,109,101,95,115,101,108,101,99,116,91,52,93,0,107,101,121,116,121,112,101,
95,101,120,116,114,101,109,101,95,115,101,108,101,99,116,91,52,93,0,107,101,121,116,121,112,101,95,98,114,101,97,107,100,111,119,110,95,115,101,108,101,99,116,91,52,93,0,107,101,121,116,121,112,101,95,106,105,116,116,101,114,95,115,101,108,101,99,116,91,52,93,0,107,101,121,116,121,112,101,95,109,111,118,101,104,111,108,100,95,115,101,108,101,99,116,91,52,93,0,107,101,121,116,121,112,101,95,103,101,110,101,114,97,116,101,100,95,115,101,108,
101,99,116,91,52,93,0,107,101,121,98,111,114,100,101,114,91,52,93,0,107,101,121,98,111,114,100,101,114,95,115,101,108,101,99,116,91,52,93,0,99,111,110,115,111,108,101,95,111,117,116,112,117,116,91,52,93,0,99,111,110,115,111,108,101,95,105,110,112,117,116,91,52,93,0,99,111,110,115,111,108,101,95,105,110,102,111,91,52,93,0,99,111,110,115,111,108,101,95,101,114,114,111,114,91,52,93,0,99,111,110,115,111,108,101,95,99,117,114,115,
111,114,91,52,93,0,99,111,110,115,111,108,101,95,115,101,108,101,99,116,91,52,93,0,118,101,114,116,101,120,95,115,105,122,101,0,101,100,103,101,95,119,105,100,116,104,0,111,98,99,101,110,116,101,114,95,100,105,97,0,102,97,99,101,100,111,116,95,115,105,122,101,0,110,111,111,100,108,101,95,99,117,114,118,105,110,103,0,103,114,105,100,95,108,101,118,101,108,115,0,100,97,115,104,95,97,108,112,104,97,0,115,121,110,116,97,120,108,91,52,
93,0,115,121,110,116,97,120,115,91,52,93,0,115,121,110,116,97,120,98,91,52,93,0,115,121,110,116,97,120,110,91,52,93,0,115,121,110,116,97,120,118,91,52,93,0,115,121,110,116,97,120,99,91,52,93,0,115,121,110,116,97,120,100,91,52,93,0,115,121,110,116,97,120,114,91,52,93,0,108,105,110,101,95,110,117,109,98,101,114,115,91,52,93,0,110,111,100,101,99,108,97,115,115,95,111,117,116,112,117,116,91,52,93,0,110,111,100,101,99,
108,97,115,115,95,102,105,108,116,101,114,91,52,93,0,110,111,100,101,99,108,97,115,115,95,118,101,99,116,111,114,91,52,93,0,110,111,100,101,99,108,97,115,115,95,116,101,120,116,117,114,101,91,52,93,0,110,111,100,101,99,108,97,115,115,95,115,104,97,100,101,114,91,52,93,0,110,111,100,101,99,108,97,115,115,95,115,99,114,105,112,116,91,52,93,0,110,111,100,101,99,108,97,115,115,95,112,97,116,116,101,114,110,91,52,93,0,110,111,100,
101,99,108,97,115,115,95,108,97,121,111,117,116,91,52,93,0,110,111,100,101,99,108,97,115,115,95,103,101,111,109,101,116,114,121,91,52,93,0,110,111,100,101,99,108,97,115,115,95,97,116,116,114,105,98,117,116,101,91,52,93,0,110,111,100,101,95,122,111,110,101,95,115,105,109,117,108,97,116,105,111,110,91,52,93,0,110,111,100,101,95,122,111,110,101,95,114,101,112,101,97,116,91,52,93,0,110,111,100,101,95,122,111,110,101,95,102,111,114,101,
97,99,104,95,103,101,111,109,101,116,114,121,95,101,108,101,109,101,110,116,91,52,93,0,110,111,100,101,95,122,111,110,101,95,99,108,111,115,117,114,101,91,52,93,0,115,105,109,117,108,97,116,101,100,95,102,114,97,109,101,115,91,52,93,0,109,111,118,105,101,91,52,93,0,109,111,118,105,101,99,108,105,112,91,52,93,0,109,97,115,107,91,52,93,0,105,109,97,103,101,91,52,93,0,115,99,101,110,101,91,52,93,0,97,117,100,105,111,91,52,
93,0,101,102,102,101,99,116,91,52,93,0,116,114,97,110,115,105,116,105,111,110,91,52,93,0,109,101,116,97,91,52,93,0,116,101,120,116,95,115,116,114,105,112,91,52,93,0,99,111,108,111,114,95,115,116,114,105,112,91,52,93,0,97,99,116,105,118,101,95,115,116,114,105,112,91,52,93,0,115,101,108,101,99,116,101,100,95,115,116,114,105,112,91,52,93,0,116,101,120,116,95,115,116,114,105,112,95,99,117,114,115,111,114,91,52,93,0,115,101,
108,101,99,116,101,100,95,116,101,120,116,91,52,93,0,107,101,121,102,114,97,109,101,95,115,99,97,108,101,95,102,97,99,0,101,100,105,116,109,101,115,104,95,97,99,116,105,118,101,91,52,93,0,104,97,110,100,108,101,95,118,101,114,116,101,120,91,52,93,0,104,97,110,100,108,101,95,118,101,114,116,101,120,95,115,101,108,101,99,116,91,52,93,0,104,97,110,100,108,101,95,118,101,114,116,101,120,95,115,105,122,101,0,99,108,105,112,112,105,110,
103,95,98,111,114,100,101,114,95,51,100,91,52,93,0,109,97,114,107,101,114,95,111,117,116,108,105,110,101,91,52,93,0,109,97,114,107,101,114,91,52,93,0,97,99,116,95,109,97,114,107,101,114,91,52,93,0,115,101,108,95,109,97,114,107,101,114,91,52,93,0,100,105,115,95,109,97,114,107,101,114,91,52,93,0,108,111,99,107,95,109,97,114,107,101,114,91,52,93,0,98,117,110,100,108,101,95,115,111,108,105,100,91,52,93,0,112,97,116,104,
95,98,101,102,111,114,101,91,52,93,0,112,97,116,104,95,97,102,116,101,114,91,52,93,0,112,97,116,104,95,107,101,121,102,114,97,109,101,95,98,101,102,111,114,101,91,52,93,0,112,97,116,104,95,107,101,121,102,114,97,109,101,95,97,102,116,101,114,91,52,93,0,99,97,109,101,114,97,95,112,97,116,104,91,52,93,0,99,97,109,101,114,97,95,112,97,115,115,101,112,97,114,116,111,117,116,91,52,93,0,103,112,95,118,101,114,116,101,120,95,
115,105,122,101,0,103,112,95,118,101,114,116,101,120,91,52,93,0,103,112,95,118,101,114,116,101,120,95,115,101,108,101,99,116,91,52,93,0,112,114,101,118,105,101,119,95,98,97,99,107,91,52,93,0,112,114,101,118,105,101,119,95,115,116,105,116,99,104,95,102,97,99,101,91,52,93,0,112,114,101,118,105,101,119,95,115,116,105,116,99,104,95,101,100,103,101,91,52,93,0,112,114,101,118,105,101,119,95,115,116,105,116,99,104,95,118,101,114,116,91,
52,93,0,112,114,101,118,105,101,119,95,115,116,105,116,99,104,95,115,116,105,116,99,104,97,98,108,101,91,52,93,0,112,114,101,118,105,101,119,95,115,116,105,116,99,104,95,117,110,115,116,105,116,99,104,97,98,108,101,91,52,93,0,112,114,101,118,105,101,119,95,115,116,105,116,99,104,95,97,99,116,105,118,101,91,52,93,0,117,118,95,115,104,97,100,111,119,91,52,93,0,109,97,116,99,104,91,52,93,0,115,101,108,101,99,116,101,100,95,104,
105,103,104,108,105,103,104,116,91,52,93,0,115,101,108,101,99,116,101,100,95,111,98,106,101,99,116,91,52,93,0,97,99,116,105,118,101,95,111,98,106,101,99,116,91,52,93,0,101,100,105,116,101,100,95,111,98,106,101,99,116,91,52,93,0,114,111,119,95,97,108,116,101,114,110,97,116,101,91,52,93,0,115,107,105,110,95,114,111,111,116,91,52,93,0,97,110,105,109,95,97,99,116,105,118,101,91,52,93,0,97,110,105,109,95,110,111,110,95,97,
99,116,105,118,101,91,52,93,0,97,110,105,109,95,112,114,101,118,105,101,119,95,114,97,110,103,101,91,52,93,0,110,108,97,95,116,119,101,97,107,105,110,103,91,52,93,0,110,108,97,95,116,119,101,97,107,100,117,112,108,105,91,52,93,0,110,108,97,95,116,114,97,99,107,91,52,93,0,110,108,97,95,116,114,97,110,115,105,116,105,111,110,91,52,93,0,110,108,97,95,116,114,97,110,115,105,116,105,111,110,95,115,101,108,91,52,93,0,110,108,
97,95,109,101,116,97,91,52,93,0,110,108,97,95,109,101,116,97,95,115,101,108,91,52,93,0,110,108,97,95,115,111,117,110,100,91,52,93,0,110,108,97,95,115,111,117,110,100,95,115,101,108,91,52,93,0,105,110,102,111,95,115,101,108,101,99,116,101,100,91,52,93,0,105,110,102,111,95,115,101,108,101,99,116,101,100,95,116,101,120,116,91,52,93,0,105,110,102,111,95,101,114,114,111,114,95,116,101,120,116,91,52,93,0,105,110,102,111,95,119,
97,114,110,105,110,103,95,116,101,120,116,91,52,93,0,105,110,102,111,95,105,110,102,111,95,116,101,120,116,91,52,93,0,105,110,102,111,95,100,101,98,117,103,91,52,93,0,105,110,102,111,95,100,101,98,117,103,95,116,101,120,116,91,52,93,0,105,110,102,111,95,112,114,111,112,101,114,116,121,91,52,93,0,105,110,102,111,95,112,114,111,112,101,114,116,121,95,116,101,120,116,91,52,93,0,105,110,102,111,95,111,112,101,114,97,116,111,114,91,52,
93,0,105,110,102,111,95,111,112,101,114,97,116,111,114,95,116,101,120,116,91,52,93,0,112,97,105,110,116,95,99,117,114,118,101,95,112,105,118,111,116,91,52,93,0,112,97,105,110,116,95,99,117,114,118,101,95,104,97,110,100,108,101,91,52,93,0,109,101,116,97,100,97,116,97,98,103,91,52,93,0,109,101,116,97,100,97,116,97,116,101,120,116,91,52,93,0,115,111,108,105,100,91,52,93,0,116,117,105,0,116,98,117,116,115,0,116,118,51,100,
0,116,102,105,108,101,0,116,105,112,111,0,116,105,110,102,111,0,116,97,99,116,0,116,110,108,97,0,116,115,101,113,0,116,105,109,97,0,116,101,120,116,0,116,111,111,112,115,0,116,110,111,100,101,0,116,117,115,101,114,112,114,101,102,0,116,99,111,110,115,111,108,101,0,116,99,108,105,112,0,116,116,111,112,98,97,114,0,116,115,116,97,116,117,115,98,97,114,0,115,112,97,99,101,95,115,112,114,101,97,100,115,104,101,101,116,0,116,97,114,
109,91,50,48,93,0,99,111,108,108,101,99,116,105,111,110,95,99,111,108,111,114,91,56,93,0,115,116,114,105,112,95,99,111,108,111,114,91,57,93,0,97,99,116,105,118,101,95,116,104,101,109,101,95,97,114,101,97,0,42,105,110,116,114,105,110,115,105,99,115,0,100,105,115,116,111,114,116,105,111,110,95,109,111,100,101,108,0,115,101,110,115,111,114,95,119,105,100,116,104,0,112,105,120,101,108,95,97,115,112,101,99,116,0,102,111,99,97,108,0,
117,110,105,116,115,0,112,114,105,110,99,105,112,97,108,95,112,111,105,110,116,91,50,93,0,112,114,105,110,99,105,112,97,108,91,50,93,0,107,49,0,107,50,0,107,51,0,100,105,118,105,115,105,111,110,95,107,49,0,100,105,118,105,115,105,111,110,95,107,50,0,110,117,107,101,95,107,49,0,110,117,107,101,95,107,50,0,98,114,111,119,110,95,107,49,0,98,114,111,119,110,95,107,50,0,98,114,111,119,110,95,107,51,0,98,114,111,119,110,95,
107,52,0,98,114,111,119,110,95,112,49,0,98,114,111,119,110,95,112,50,0,112,111,115,91,50,93,0,112,97,116,116,101,114,110,95,99,111,114,110,101,114,115,91,52,93,91,50,93,0,115,101,97,114,99,104,95,109,105,110,91,50,93,0,115,101,97,114,99,104,95,109,97,120,91,50,93,0,112,97,116,95,109,105,110,91,50,93,0,112,97,116,95,109,97,120,91,50,93,0,109,97,114,107,101,114,115,110,114,0,42,109,97,114,107,101,114,115,0,98,
117,110,100,108,101,95,112,111,115,91,51,93,0,112,97,116,95,102,108,97,103,0,115,101,97,114,99,104,95,102,108,97,103,0,102,114,97,109,101,115,95,108,105,109,105,116,0,112,97,116,116,101,114,110,95,109,97,116,99,104,0,109,111,116,105,111,110,95,109,111,100,101,108,0,97,108,103,111,114,105,116,104,109,95,102,108,97,103,0,109,105,110,105,109,117,109,95,99,111,114,114,101,108,97,116,105,111,110,0,119,101,105,103,104,116,95,115,116,97,98,
0,99,111,114,110,101,114,115,91,52,93,91,50,93,0,42,42,112,111,105,110,116,95,116,114,97,99,107,115,0,112,111,105,110,116,95,116,114,97,99,107,115,110,114,0,105,109,97,103,101,95,111,112,97,99,105,116,121,0,108,97,115,116,95,109,97,114,107,101,114,0,100,101,102,97,117,108,116,95,109,111,116,105,111,110,95,109,111,100,101,108,0,100,101,102,97,117,108,116,95,97,108,103,111,114,105,116,104,109,95,102,108,97,103,0,100,101,102,97,117,
108,116,95,109,105,110,105,109,117,109,95,99,111,114,114,101,108,97,116,105,111,110,0,100,101,102,97,117,108,116,95,112,97,116,116,101,114,110,95,115,105,122,101,0,100,101,102,97,117,108,116,95,115,101,97,114,99,104,95,115,105,122,101,0,100,101,102,97,117,108,116,95,102,114,97,109,101,115,95,108,105,109,105,116,0,100,101,102,97,117,108,116,95,109,97,114,103,105,110,0,100,101,102,97,117,108,116,95,112,97,116,116,101,114,110,95,109,97,116,99,
104,0,100,101,102,97,117,108,116,95,102,108,97,103,0,109,111,116,105,111,110,95,102,108,97,103,0,107,101,121,102,114,97,109,101,49,0,107,101,121,102,114,97,109,101,50,0,114,101,99,111,110,115,116,114,117,99,116,105,111,110,95,102,108,97,103,0,114,101,102,105,110,101,95,99,97,109,101,114,97,95,105,110,116,114,105,110,115,105,99,115,0,99,108,101,97,110,95,102,114,97,109,101,115,0,99,108,101,97,110,95,97,99,116,105,111,110,0,99,108,
101,97,110,95,101,114,114,111,114,0,111,98,106,101,99,116,95,100,105,115,116,97,110,99,101,0,116,111,116,95,116,114,97,99,107,0,97,99,116,95,116,114,97,99,107,0,116,111,116,95,114,111,116,95,116,114,97,99,107,0,97,99,116,95,114,111,116,95,116,114,97,99,107,0,109,97,120,115,99,97,108,101,0,42,114,111,116,95,116,114,97,99,107,0,97,110,99,104,111,114,95,102,114,97,109,101,0,116,97,114,103,101,116,95,112,111,115,91,50,93,
0,116,97,114,103,101,116,95,114,111,116,0,108,111,99,105,110,102,0,115,99,97,108,101,105,110,102,0,114,111,116,105,110,102,0,108,97,115,116,95,99,97,109,101,114,97,0,99,97,109,110,114,0,42,99,97,109,101,114,97,115,0,116,114,97,99,107,115,0,112,108,97,110,101,95,116,114,97,99,107,115,0,42,97,99,116,105,118,101,95,116,114,97,99,107,0,42,97,99,116,105,118,101,95,112,108,97,110,101,95,116,114,97,99,107,0,114,101,99,111,
110,115,116,114,117,99,116,105,111,110,0,109,101,115,115,97,103,101,91,50,53,54,93,0,116,111,116,95,115,101,103,109,101,110,116,0,109,97,120,95,115,101,103,109,101,110,116,0,116,111,116,97,108,95,102,114,97,109,101,115,0,102,105,114,115,116,95,110,111,116,95,100,105,115,97,98,108,101,100,95,109,97,114,107,101,114,95,102,114,97,109,101,110,114,0,108,97,115,116,95,110,111,116,95,100,105,115,97,98,108,101,100,95,109,97,114,107,101,114,95,
102,114,97,109,101,110,114,0,99,111,118,101,114,97,103,101,0,115,111,114,116,95,109,101,116,104,111,100,0,99,111,118,101,114,97,103,101,95,115,101,103,109,101,110,116,115,0,116,111,116,95,99,104,97,110,110,101,108,0,99,97,109,101,114,97,0,115,116,97,98,105,108,105,122,97,116,105,111,110,0,42,97,99,116,95,112,108,97,110,101,95,116,114,97,99,107,0,111,98,106,101,99,116,115,0,111,98,106,101,99,116,110,114,0,116,111,116,95,111,98,
106,101,99,116,0,100,111,112,101,115,104,101,101,116,0,109,111,100,117,108,101,91,49,50,56,93,0,112,97,116,104,91,55,54,56,93,0,115,112,97,99,101,95,116,121,112,101,0,99,111,110,116,101,120,116,91,54,52,93,0,105,116,101,109,115,0,117,105,95,110,97,109,101,91,54,52,93,0,111,112,95,105,100,110,97,109,101,91,54,52,93,0,111,112,95,112,114,111,112,95,101,110,117,109,91,54,52,93,0,111,112,99,111,110,116,101,120,116,0,109,
116,95,105,100,110,97,109,101,91,54,52,93,0,99,111,110,116,101,120,116,95,100,97,116,97,95,112,97,116,104,91,50,53,54,93,0,112,114,111,112,95,105,100,91,54,52,93,0,112,114,111,112,95,105,110,100,101,120,0,105,109,112,111,114,116,95,109,101,116,104,111,100,0,109,111,100,117,108,101,91,52,56,93,0,42,97,99,99,101,115,115,95,116,111,107,101,110,0,99,117,115,116,111,109,95,100,105,114,112,97,116,104,91,49,48,50,52,93,0,114,
101,109,111,116,101,95,117,114,108,91,49,48,50,52,93,0,115,109,111,111,116,104,0,115,112,101,99,91,52,93,0,109,111,117,115,101,95,115,112,101,101,100,0,119,97,108,107,95,115,112,101,101,100,0,119,97,108,107,95,115,112,101,101,100,95,102,97,99,116,111,114,0,118,105,101,119,95,104,101,105,103,104,116,0,106,117,109,112,95,104,101,105,103,104,116,0,116,101,108,101,112,111,114,116,95,116,105,109,101,0,105,115,95,100,105,114,116,121,0,115,
101,99,116,105,111,110,95,97,99,116,105,118,101,0,100,105,115,112,108,97,121,95,116,121,112,101,0,115,111,114,116,95,116,121,112,101,0,116,101,109,112,95,119,105,110,95,115,105,122,101,120,0,116,101,109,112,95,119,105,110,95,115,105,122,101,121,0,117,115,101,95,117,110,100,111,95,108,101,103,97,99,121,0,110,111,95,111,118,101,114,114,105,100,101,95,97,117,116,111,95,114,101,115,121,110,99,0,117,115,101,95,99,121,99,108,101,115,95,100,101,
98,117,103,0,117,115,101,95,101,101,118,101,101,95,100,101,98,117,103,0,115,104,111,119,95,97,115,115,101,116,95,100,101,98,117,103,95,105,110,102,111,0,110,111,95,97,115,115,101,116,95,105,110,100,101,120,105,110,103,0,117,115,101,95,118,105,101,119,112,111,114,116,95,100,101,98,117,103,0,117,115,101,95,97,108,108,95,108,105,110,107,101,100,95,100,97,116,97,95,100,105,114,101,99,116,0,117,115,101,95,101,120,116,101,110,115,105,111,110,115,
95,100,101,98,117,103,0,117,115,101,95,114,101,99,111,109,112,117,116,101,95,117,115,101,114,99,111,117,110,116,95,111,110,95,115,97,118,101,95,100,101,98,117,103,0,119,114,105,116,101,95,108,97,114,103,101,95,98,108,101,110,100,95,102,105,108,101,95,98,108,111,99,107,115,0,117,115,101,95,97,116,116,114,105,98,117,116,101,95,115,116,111,114,97,103,101,95,119,114,105,116,101,0,83,65,78,73,84,73,90,69,95,65,70,84,69,82,95,72,69,
82,69,0,117,115,101,95,110,101,119,95,99,117,114,118,101,115,95,116,111,111,108,115,0,117,115,101,95,101,120,116,101,110,100,101,100,95,97,115,115,101,116,95,98,114,111,119,115,101,114,0,117,115,101,95,115,99,117,108,112,116,95,116,101,120,116,117,114,101,95,112,97,105,110,116,0,117,115,101,95,110,101,119,95,118,111,108,117,109,101,95,110,111,100,101,115,0,117,115,101,95,115,104,97,100,101,114,95,110,111,100,101,95,112,114,101,118,105,101,119,
115,0,117,115,101,95,98,117,110,100,108,101,95,97,110,100,95,99,108,111,115,117,114,101,95,110,111,100,101,115,0,117,115,101,95,115,111,99,107,101,116,95,115,116,114,117,99,116,117,114,101,95,116,121,112,101,0,100,105,114,95,112,97,116,104,91,55,54,56,93,0,115,104,101,108,102,95,105,100,110,97,109,101,91,54,52,93,0,118,101,114,115,105,111,110,102,105,108,101,0,115,117,98,118,101,114,115,105,111,110,102,105,108,101,0,100,117,112,102,108,
97,103,0,112,114,101,102,95,102,108,97,103,0,115,97,118,101,116,105,109,101,0,109,111,117,115,101,95,101,109,117,108,97,116,101,95,51,95,98,117,116,116,111,110,95,109,111,100,105,102,105,101,114,0,116,114,97,99,107,112,97,100,95,115,99,114,111,108,108,95,100,105,114,101,99,116,105,111,110,0,116,101,109,112,100,105,114,91,55,54,56,93,0,102,111,110,116,100,105,114,91,55,54,56,93,0,114,101,110,100,101,114,100,105,114,91,49,48,50,52,
93,0,114,101,110,100,101,114,95,99,97,99,104,101,100,105,114,91,55,54,56,93,0,116,101,120,116,117,100,105,114,91,55,54,56,93,0,112,121,116,104,111,110,100,105,114,91,55,54,56,93,0,115,111,117,110,100,100,105,114,91,55,54,56,93,0,105,49,56,110,100,105,114,91,55,54,56,93,0,105,109,97,103,101,95,101,100,105,116,111,114,91,49,48,50,52,93,0,116,101,120,116,95,101,100,105,116,111,114,91,49,48,50,52,93,0,116,101,120,116,
95,101,100,105,116,111,114,95,97,114,103,115,91,50,53,54,93,0,97,110,105,109,95,112,108,97,121,101,114,91,49,48,50,52,93,0,97,110,105,109,95,112,108,97,121,101,114,95,112,114,101,115,101,116,0,118,50,100,95,109,105,110,95,103,114,105,100,115,105,122,101,0,116,105,109,101,99,111,100,101,95,115,116,121,108,101,0,118,101,114,115,105,111,110,115,0,100,98,108,95,99,108,105,99,107,95,116,105,109,101,0,98,111,114,100,101,114,95,119,105,
100,116,104,0,109,105,110,105,95,97,120,105,115,95,116,121,112,101,0,117,105,102,108,97,103,0,117,105,102,108,97,103,50,0,103,112,117,95,102,108,97,103,0,95,112,97,100,56,91,54,93,0,97,112,112,95,102,108,97,103,0,118,105,101,119,122,111,111,109,0,108,97,110,103,117,97,103,101,0,109,105,120,98,117,102,115,105,122,101,0,97,117,100,105,111,100,101,118,105,99,101,0,97,117,100,105,111,114,97,116,101,0,97,117,100,105,111,102,111,114,
109,97,116,0,97,117,100,105,111,99,104,97,110,110,101,108,115,0,117,105,95,115,99,97,108,101,0,117,105,95,108,105,110,101,95,119,105,100,116,104,0,100,112,105,0,115,99,97,108,101,95,102,97,99,116,111,114,0,105,110,118,95,115,99,97,108,101,95,102,97,99,116,111,114,0,112,105,120,101,108,115,105,122,101,0,118,105,114,116,117,97,108,95,112,105,120,101,108,0,110,111,100,101,95,109,97,114,103,105,110,0,110,111,100,101,95,112,114,101,118,
105,101,119,95,114,101,115,0,116,114,97,110,115,111,112,116,115,0,109,101,110,117,116,104,114,101,115,104,111,108,100,49,0,109,101,110,117,116,104,114,101,115,104,111,108,100,50,0,97,112,112,95,116,101,109,112,108,97,116,101,91,54,52,93,0,116,104,101,109,101,115,0,117,105,102,111,110,116,115,0,117,105,115,116,121,108,101,115,0,117,115,101,114,95,107,101,121,109,97,112,115,0,117,115,101,114,95,107,101,121,99,111,110,102,105,103,95,112,114,101,
102,115,0,97,100,100,111,110,115,0,97,117,116,111,101,120,101,99,95,112,97,116,104,115,0,115,99,114,105,112,116,95,100,105,114,101,99,116,111,114,105,101,115,0,117,115,101,114,95,109,101,110,117,115,0,97,115,115,101,116,95,108,105,98,114,97,114,105,101,115,0,101,120,116,101,110,115,105,111,110,95,114,101,112,111,115,0,97,115,115,101,116,95,115,104,101,108,118,101,115,95,115,101,116,116,105,110,103,115,0,107,101,121,99,111,110,102,105,103,115,
116,114,91,54,52,93,0,97,99,116,105,118,101,95,97,115,115,101,116,95,108,105,98,114,97,114,121,0,97,99,116,105,118,101,95,101,120,116,101,110,115,105,111,110,95,114,101,112,111,0,101,120,116,101,110,115,105,111,110,95,102,108,97,103,0,110,101,116,119,111,114,107,95,116,105,109,101,111,117,116,0,110,101,116,119,111,114,107,95,99,111,110,110,101,99,116,105,111,110,95,108,105,109,105,116,0,95,112,97,100,49,52,91,51,93,0,117,110,100,111,
115,116,101,112,115,0,117,110,100,111,109,101,109,111,114,121,0,103,112,117,95,118,105,101,119,112,111,114,116,95,113,117,97,108,105,116,121,0,103,112,95,109,97,110,104,97,116,116,101,110,100,105,115,116,0,103,112,95,101,117,99,108,105,100,101,97,110,100,105,115,116,0,103,112,95,101,114,97,115,101,114,0,103,112,95,115,101,116,116,105,110,103,115,0,95,112,97,100,49,51,91,52,93,0,108,105,103,104,116,95,112,97,114,97,109,91,52,93,0,108,
105,103,104,116,95,97,109,98,105,101,110,116,91,51,93,0,103,105,122,109,111,95,115,105,122,101,0,103,105,122,109,111,95,115,105,122,101,95,110,97,118,105,103,97,116,101,95,118,51,100,0,95,112,97,100,51,91,53,93,0,101,100,105,116,95,115,116,117,100,105,111,95,108,105,103,104,116,0,108,111,111,107,100,101,118,95,115,112,104,101,114,101,95,115,105,122,101,0,118,98,111,116,105,109,101,111,117,116,0,118,98,111,99,111,108,108,101,99,116,114,
97,116,101,0,116,101,120,116,105,109,101,111,117,116,0,116,101,120,99,111,108,108,101,99,116,114,97,116,101,0,109,101,109,99,97,99,104,101,108,105,109,105,116,0,112,114,101,102,101,116,99,104,102,114,97,109,101,115,0,112,97,100,95,114,111,116,95,97,110,103,108,101,0,114,118,105,115,105,122,101,0,114,118,105,98,114,105,103,104,116,0,114,101,99,101,110,116,95,102,105,108,101,115,0,115,109,111,111,116,104,95,118,105,101,119,116,120,0,103,108,
114,101,115,108,105,109,105,116,0,99,111,108,111,114,95,112,105,99,107,101,114,95,116,121,112,101,0,97,117,116,111,95,115,109,111,111,116,104,105,110,103,95,110,101,119,0,105,112,111,95,110,101,119,0,107,101,121,104,97,110,100,108,101,115,95,110,101,119,0,95,112,97,100,49,49,91,52,93,0,118,105,101,119,95,102,114,97,109,101,95,116,121,112,101,0,118,105,101,119,95,102,114,97,109,101,95,107,101,121,102,114,97,109,101,115,0,118,105,101,119,
95,102,114,97,109,101,95,115,101,99,111,110,100,115,0,103,112,117,95,112,114,101,102,101,114,114,101,100,95,105,110,100,101,120,0,103,112,117,95,112,114,101,102,101,114,114,101,100,95,118,101,110,100,111,114,95,105,100,0,103,112,117,95,112,114,101,102,101,114,114,101,100,95,100,101,118,105,99,101,95,105,100,0,109,97,120,95,115,104,97,100,101,114,95,99,111,109,112,105,108,97,116,105,111,110,95,115,117,98,112,114,111,99,101,115,115,101,115,0,115,
104,97,100,101,114,95,99,111,109,112,105,108,97,116,105,111,110,95,109,101,116,104,111,100,0,95,112,97,100,49,54,91,50,93,0,103,112,117,95,98,97,99,107,101,110,100,0,112,108,97,121,98,97,99,107,95,102,112,115,95,115,97,109,112,108,101,115,0,119,105,100,103,101,116,95,117,110,105,116,0,97,110,105,115,111,116,114,111,112,105,99,95,102,105,108,116,101,114,0,116,97,98,108,101,116,95,97,112,105,0,112,114,101,115,115,117,114,101,95,116,
104,114,101,115,104,111,108,100,95,109,97,120,0,112,114,101,115,115,117,114,101,95,115,111,102,116,110,101,115,115,0,110,100,111,102,95,115,101,110,115,105,116,105,118,105,116,121,0,110,100,111,102,95,111,114,98,105,116,95,115,101,110,115,105,116,105,118,105,116,121,0,110,100,111,102,95,100,101,97,100,122,111,110,101,0,110,100,111,102,95,102,108,97,103,0,110,100,111,102,95,110,97,118,105,103,97,116,105,111,110,95,109,111,100,101,0,95,112,97,100,
49,55,91,49,93,0,105,109,97,103,101,95,100,114,97,119,95,109,101,116,104,111,100,0,103,108,97,108,112,104,97,99,108,105,112,0,97,117,116,111,107,101,121,95,102,108,97,103,0,107,101,121,95,105,110,115,101,114,116,95,99,104,97,110,110,101,108,115,0,95,112,97,100,49,53,91,54,93,0,97,110,105,109,97,116,105,111,110,95,102,108,97,103,0,116,101,120,116,95,114,101,110,100,101,114,0,110,97,118,105,103,97,116,105,111,110,95,109,111,100,
101,0,118,105,101,119,95,114,111,116,97,116,101,95,115,101,110,115,105,116,105,118,105,116,121,95,116,117,114,110,116,97,98,108,101,0,118,105,101,119,95,114,111,116,97,116,101,95,115,101,110,115,105,116,105,118,105,116,121,95,116,114,97,99,107,98,97,108,108,0,99,111,98,97,95,119,101,105,103,104,116,0,115,99,117,108,112,116,95,112,97,105,110,116,95,111,118,101,114,108,97,121,95,99,111,108,91,51,93,0,103,112,101,110,99,105,108,95,110,101,
119,95,108,97,121,101,114,95,99,111,108,91,52,93,0,100,114,97,103,95,116,104,114,101,115,104,111,108,100,95,109,111,117,115,101,0,100,114,97,103,95,116,104,114,101,115,104,111,108,100,95,116,97,98,108,101,116,0,100,114,97,103,95,116,104,114,101,115,104,111,108,100,0,109,111,118,101,95,116,104,114,101,115,104,111,108,100,0,102,111,110,116,95,112,97,116,104,95,117,105,91,49,48,50,52,93,0,102,111,110,116,95,112,97,116,104,95,117,105,95,
109,111,110,111,91,49,48,50,52,93,0,99,111,109,112,117,116,101,95,100,101,118,105,99,101,95,116,121,112,101,0,102,99,117,95,105,110,97,99,116,105,118,101,95,97,108,112,104,97,0,112,105,101,95,116,97,112,95,116,105,109,101,111,117,116,0,112,105,101,95,105,110,105,116,105,97,108,95,116,105,109,101,111,117,116,0,112,105,101,95,97,110,105,109,97,116,105,111,110,95,116,105,109,101,111,117,116,0,112,105,101,95,109,101,110,117,95,99,111,110,
102,105,114,109,0,112,105,101,95,109,101,110,117,95,114,97,100,105,117,115,0,112,105,101,95,109,101,110,117,95,116,104,114,101,115,104,111,108,100,0,115,101,113,117,101,110,99,101,114,95,101,100,105,116,111,114,95,102,108,97,103,0,102,97,99,116,111,114,95,100,105,115,112,108,97,121,95,116,121,112,101,0,114,101,110,100,101,114,95,100,105,115,112,108,97,121,95,116,121,112,101,0,102,105,108,101,98,114,111,119,115,101,114,95,100,105,115,112,108,97,
121,95,116,121,112,101,0,115,101,113,117,101,110,99,101,114,95,112,114,111,120,121,95,115,101,116,117,112,0,99,111,108,108,101,99,116,105,111,110,95,105,110,115,116,97,110,99,101,95,101,109,112,116,121,95,115,105,122,101,0,116,101,120,116,95,102,108,97,103,0,95,112,97,100,49,48,91,49,93,0,102,105,108,101,95,112,114,101,118,105,101,119,95,116,121,112,101,0,115,116,97,116,117,115,98,97,114,95,102,108,97,103,0,119,97,108,107,95,110,97,
118,105,103,97,116,105,111,110,0,115,112,97,99,101,95,100,97,116,97,0,102,105,108,101,95,115,112,97,99,101,95,100,97,116,97,0,101,120,112,101,114,105,109,101,110,116,97,108,0,116,105,109,101,95,108,111,119,0,116,105,109,101,95,109,105,100,0,116,105,109,101,95,104,105,95,97,110,100,95,118,101,114,115,105,111,110,0,99,108,111,99,107,95,115,101,113,95,104,105,95,97,110,100,95,114,101,115,101,114,118,101,100,0,99,108,111,99,107,95,115,
101,113,95,108,111,119,0,110,111,100,101,91,54,93,0,118,97,108,117,101,91,52,93,91,52,93,0,116,114,97,110,115,91,52,93,0,115,99,97,108,101,91,52,93,91,52,93,0,115,99,97,108,101,95,119,101,105,103,104,116,0,42,116,101,109,112,95,112,102,0,118,101,114,116,0,104,111,114,0,109,97,115,107,0,109,105,110,91,50,93,0,109,97,120,91,50,93,0,109,105,110,122,111,111,109,0,109,97,120,122,111,111,109,0,115,99,114,111,108,108,
0,115,99,114,111,108,108,95,117,105,0,107,101,101,112,116,111,116,0,107,101,101,112,122,111,111,109,0,107,101,101,112,111,102,115,0,111,108,100,119,105,110,120,0,111,108,100,119,105,110,121,0,97,108,112,104,97,95,118,101,114,116,0,97,108,112,104,97,95,104,111,114,0,112,97,103,101,95,115,105,122,101,95,121,0,42,115,109,115,0,42,115,109,111,111,116,104,95,116,105,109,101,114,0,119,105,110,109,97,116,91,52,93,91,52,93,0,118,105,101,
119,109,97,116,91,52,93,91,52,93,0,118,105,101,119,105,110,118,91,52,93,91,52,93,0,112,101,114,115,109,97,116,91,52,93,91,52,93,0,112,101,114,115,105,110,118,91,52,93,91,52,93,0,118,105,101,119,99,97,109,116,101,120,99,111,102,97,99,91,52,93,0,118,105,101,119,109,97,116,111,98,91,52,93,91,52,93,0,112,101,114,115,109,97,116,111,98,91,52,93,91,52,93,0,99,108,105,112,91,54,93,91,52,93,0,99,108,105,112,95,
108,111,99,97,108,91,54,93,91,52,93,0,42,99,108,105,112,98,98,0,42,108,111,99,97,108,118,100,0,42,118,105,101,119,95,114,101,110,100,101,114,0,116,119,109,97,116,91,52,93,91,52,93,0,116,119,95,97,120,105,115,95,109,105,110,91,51,93,0,116,119,95,97,120,105,115,95,109,97,120,91,51,93,0,116,119,95,97,120,105,115,95,109,97,116,114,105,120,91,51,93,91,51,93,0,103,114,105,100,118,105,101,119,0,118,105,101,119,113,117,
97,116,91,52,93,0,99,97,109,100,120,0,99,97,109,100,121,0,112,105,120,115,105,122,101,0,99,97,109,122,111,111,109,0,105,115,95,112,101,114,115,112,0,112,101,114,115,112,0,118,105,101,119,95,97,120,105,115,95,114,111,108,108,0,118,105,101,119,108,111,99,107,0,114,117,110,116,105,109,101,95,118,105,101,119,108,111,99,107,0,118,105,101,119,108,111,99,107,95,113,117,97,100,0,111,102,115,95,108,111,99,107,91,50,93,0,116,119,100,114,
97,119,102,108,97,103,0,114,102,108,97,103,0,108,118,105,101,119,113,117,97,116,91,52,93,0,108,112,101,114,115,112,0,108,118,105,101,119,0,108,118,105,101,119,95,97,120,105,115,95,114,111,108,108,0,110,100,111,102,95,111,102,115,91,51,93,0,110,100,111,102,95,114,111,116,95,97,110,103,108,101,0,110,100,111,102,95,114,111,116,95,97,120,105,115,91,51,93,0,114,111,116,97,116,105,111,110,95,113,117,97,116,101,114,110,105,111,110,91,52,
93,0,114,111,116,97,116,105,111,110,95,97,120,105,115,91,51,93,0,114,111,116,97,116,105,111,110,95,97,110,103,108,101,0,112,114,101,118,95,116,121,112,101,0,112,114,101,118,95,116,121,112,101,95,119,105,114,101,0,99,111,108,111,114,95,116,121,112,101,0,108,105,103,104,116,0,98,97,99,107,103,114,111,117,110,100,95,116,121,112,101,0,99,97,118,105,116,121,95,116,121,112,101,0,119,105,114,101,95,99,111,108,111,114,95,116,121,112,101,0,
117,115,101,95,99,111,109,112,111,115,105,116,111,114,0,115,116,117,100,105,111,95,108,105,103,104,116,91,50,53,54,93,0,108,111,111,107,100,101,118,95,108,105,103,104,116,91,50,53,54,93,0,109,97,116,99,97,112,91,50,53,54,93,0,115,104,97,100,111,119,95,105,110,116,101,110,115,105,116,121,0,115,105,110,103,108,101,95,99,111,108,111,114,91,51,93,0,115,116,117,100,105,111,108,105,103,104,116,95,114,111,116,95,122,0,115,116,117,100,105,
111,108,105,103,104,116,95,98,97,99,107,103,114,111,117,110,100,0,115,116,117,100,105,111,108,105,103,104,116,95,105,110,116,101,110,115,105,116,121,0,115,116,117,100,105,111,108,105,103,104,116,95,98,108,117,114,0,111,98,106,101,99,116,95,111,117,116,108,105,110,101,95,99,111,108,111,114,91,51,93,0,120,114,97,121,95,97,108,112,104,97,0,120,114,97,121,95,97,108,112,104,97,95,119,105,114,101,0,99,97,118,105,116,121,95,118,97,108,108,101,
121,95,102,97,99,116,111,114,0,99,97,118,105,116,121,95,114,105,100,103,101,95,102,97,99,116,111,114,0,98,97,99,107,103,114,111,117,110,100,95,99,111,108,111,114,91,51,93,0,99,117,114,118,97,116,117,114,101,95,114,105,100,103,101,95,102,97,99,116,111,114,0,99,117,114,118,97,116,117,114,101,95,118,97,108,108,101,121,95,102,97,99,116,111,114,0,114,101,110,100,101,114,95,112,97,115,115,0,97,111,118,95,110,97,109,101,91,54,52,93,
0,101,100,105,116,95,102,108,97,103,0,110,111,114,109,97,108,115,95,108,101,110,103,116,104,0,110,111,114,109,97,108,115,95,99,111,110,115,116,97,110,116,95,115,99,114,101,101,110,95,115,105,122,101,0,112,97,105,110,116,95,102,108,97,103,0,119,112,97,105,110,116,95,102,108,97,103,0,116,101,120,116,117,114,101,95,112,97,105,110,116,95,109,111,100,101,95,111,112,97,99,105,116,121,0,118,101,114,116,101,120,95,112,97,105,110,116,95,109,111,
100,101,95,111,112,97,99,105,116,121,0,119,101,105,103,104,116,95,112,97,105,110,116,95,109,111,100,101,95,111,112,97,99,105,116,121,0,115,99,117,108,112,116,95,109,111,100,101,95,109,97,115,107,95,111,112,97,99,105,116,121,0,115,99,117,108,112,116,95,109,111,100,101,95,102,97,99,101,95,115,101,116,115,95,111,112,97,99,105,116,121,0,118,105,101,119,101,114,95,97,116,116,114,105,98,117,116,101,95,111,112,97,99,105,116,121,0,120,114,97,
121,95,97,108,112,104,97,95,98,111,110,101,0,98,111,110,101,95,119,105,114,101,95,97,108,112,104,97,0,102,97,100,101,95,97,108,112,104,97,0,119,105,114,101,102,114,97,109,101,95,116,104,114,101,115,104,111,108,100,0,119,105,114,101,102,114,97,109,101,95,111,112,97,99,105,116,121,0,114,101,116,111,112,111,108,111,103,121,95,111,102,102,115,101,116,0,103,112,101,110,99,105,108,95,112,97,112,101,114,95,111,112,97,99,105,116,121,0,103,112,
101,110,99,105,108,95,103,114,105,100,95,111,112,97,99,105,116,121,0,103,112,101,110,99,105,108,95,102,97,100,101,95,108,97,121,101,114,0,103,112,101,110,99,105,108,95,103,114,105,100,95,99,111,108,111,114,91,51,93,0,103,112,101,110,99,105,108,95,103,114,105,100,95,115,99,97,108,101,91,50,93,0,103,112,101,110,99,105,108,95,103,114,105,100,95,111,102,102,115,101,116,91,50,93,0,103,112,101,110,99,105,108,95,103,114,105,100,95,115,117,
98,100,105,118,105,115,105,111,110,115,0,103,112,101,110,99,105,108,95,118,101,114,116,101,120,95,112,97,105,110,116,95,111,112,97,99,105,116,121,0,104,97,110,100,108,101,95,100,105,115,112,108,97,121,0,115,99,117,108,112,116,95,99,117,114,118,101,115,95,99,97,103,101,95,111,112,97,99,105,116,121,0,42,112,114,111,112,101,114,116,105,101,115,95,115,116,111,114,97,103,101,0,40,42,112,114,111,112,101,114,116,105,101,115,95,115,116,111,114,97,
103,101,95,102,114,101,101,41,40,41,0,42,108,111,99,97,108,95,115,116,97,116,115,0,98,117,110,100,108,101,95,115,105,122,101,0,98,117,110,100,108,101,95,100,114,97,119,116,121,112,101,0,111,98,106,101,99,116,95,116,121,112,101,95,101,120,99,108,117,100,101,95,118,105,101,119,112,111,114,116,0,111,98,106,101,99,116,95,116,121,112,101,95,101,120,99,108,117,100,101,95,115,101,108,101,99,116,0,42,111,98,95,99,101,110,116,114,101,0,114,
101,110,100,101,114,95,98,111,114,100,101,114,0,111,98,95,99,101,110,116,114,101,95,98,111,110,101,91,54,52,93,0,108,111,99,97,108,95,118,105,101,119,95,117,117,105,100,0,108,111,99,97,108,95,99,111,108,108,101,99,116,105,111,110,115,95,117,117,105,100,0,100,101,98,117,103,95,102,108,97,103,0,111,98,95,99,101,110,116,114,101,95,99,117,114,115,111,114,0,115,99,101,110,101,108,111,99,107,0,103,112,95,102,108,97,103,0,110,101,97,
114,0,102,97,114,0,103,105,122,109,111,95,115,104,111,119,95,111,98,106,101,99,116,0,103,105,122,109,111,95,115,104,111,119,95,97,114,109,97,116,117,114,101,0,103,105,122,109,111,95,115,104,111,119,95,101,109,112,116,121,0,103,105,122,109,111,95,115,104,111,119,95,108,105,103,104,116,0,103,105,122,109,111,95,115,104,111,119,95,99,97,109,101,114,97,0,103,114,105,100,102,108,97,103,0,103,114,105,100,108,105,110,101,115,0,103,114,105,100,115,
117,98,100,105,118,0,118,101,114,116,101,120,95,111,112,97,99,105,116,121,0,115,116,101,114,101,111,51,100,95,102,108,97,103,0,115,116,101,114,101,111,51,100,95,99,97,109,101,114,97,0,115,116,101,114,101,111,51,100,95,99,111,110,118,101,114,103,101,110,99,101,95,102,97,99,116,111,114,0,115,116,101,114,101,111,51,100,95,118,111,108,117,109,101,95,97,108,112,104,97,0,115,116,101,114,101,111,51,100,95,99,111,110,118,101,114,103,101,110,99,
101,95,97,108,112,104,97,0,42,117,105,95,110,97,109,101,0,109,111,100,105,102,105,101,114,95,117,105,100,0,115,105,109,95,111,117,116,112,117,116,95,110,111,100,101,95,105,100,0,114,101,112,101,97,116,95,111,117,116,112,117,116,95,110,111,100,101,95,105,100,0,105,116,101,114,97,116,105,111,110,0,122,111,110,101,95,111,117,116,112,117,116,95,110,111,100,101,95,105,100,0,101,118,97,108,117,97,116,101,95,110,111,100,101,95,105,100,0,115,111,
117,114,99,101,95,111,117,116,112,117,116,95,110,111,100,101,95,105,100,0,42,115,111,117,114,99,101,95,110,111,100,101,95,116,114,101,101,0,119,105,114,101,102,114,97,109,101,95,116,121,112,101,0,119,105,114,101,102,114,97,109,101,95,100,101,116,97,105,108,0,105,110,116,101,114,112,111,108,97,116,105,111,110,95,109,101,116,104,111,100,0,115,101,113,117,101,110,99,101,95,109,111,100,101,0,102,114,97,109,101,95,100,117,114,97,116,105,111,110,0,
97,99,116,105,118,101,95,103,114,105,100,0,114,101,110,100,101,114,0,118,101,108,111,99,105,116,121,95,103,114,105,100,91,54,52,93,0,108,105,115,116,0,112,114,105,110,116,108,101,118,101,108,0,115,116,111,114,101,108,101,118,101,108,0,42,114,101,112,111,114,116,116,105,109,101,114,0,42,108,111,99,107,0,115,101,115,115,105,111,110,95,115,101,116,116,105,110,103,115,0,42,119,105,110,100,114,97,119,97,98,108,101,0,42,119,105,110,97,99,116,
105,118,101,0,119,105,110,100,111,119,115,0,105,110,105,116,95,102,108,97,103,0,102,105,108,101,95,115,97,118,101,100,0,111,112,95,117,110,100,111,95,100,101,112,116,104,0,111,117,116,108,105,110,101,114,95,115,121,110,99,95,115,101,108,101,99,116,95,100,105,114,116,121,0,111,112,101,114,97,116,111,114,115,0,101,120,116,101,110,115,105,111,110,115,95,117,112,100,97,116,101,115,0,101,120,116,101,110,115,105,111,110,115,95,98,108,111,99,107,101,
100,0,106,111,98,115,0,112,97,105,110,116,99,117,114,115,111,114,115,0,100,114,97,103,115,0,107,101,121,99,111,110,102,105,103,115,0,42,100,101,102,97,117,108,116,99,111,110,102,0,42,97,100,100,111,110,99,111,110,102,0,42,117,115,101,114,99,111,110,102,0,116,105,109,101,114,115,0,42,97,117,116,111,115,97,118,101,116,105,109,101,114,0,97,117,116,111,115,97,118,101,95,115,99,104,101,100,117,108,101,100,0,42,117,110,100,111,95,115,116,
97,99,107,0,42,109,101,115,115,97,103,101,95,98,117,115,0,120,114,0,42,103,104,111,115,116,119,105,110,0,42,103,112,117,99,116,120,0,42,110,101,119,95,115,99,101,110,101,0,118,105,101,119,95,108,97,121,101,114,95,110,97,109,101,91,54,52,93,0,42,117,110,112,105,110,110,101,100,95,115,99,101,110,101,0,42,119,111,114,107,115,112,97,99,101,95,104,111,111,107,0,103,108,111,98,97,108,95,97,114,101,97,95,109,97,112,0,42,115,99,
114,101,101,110,0,112,111,115,120,0,112,111,115,121,0,119,105,110,100,111,119,115,116,97,116,101,0,108,97,115,116,99,117,114,115,111,114,0,109,111,100,97,108,99,117,114,115,111,114,0,103,114,97,98,99,117,114,115,111,114,0,112,105,101,95,101,118,101,110,116,95,116,121,112,101,95,108,111,99,107,0,112,105,101,95,101,118,101,110,116,95,116,121,112,101,95,108,97,115,116,0,116,97,103,95,99,117,114,115,111,114,95,114,101,102,114,101,115,104,0,
101,118,101,110,116,95,113,117,101,117,101,95,99,104,101,99,107,95,99,108,105,99,107,0,101,118,101,110,116,95,113,117,101,117,101,95,99,104,101,99,107,95,100,114,97,103,0,101,118,101,110,116,95,113,117,101,117,101,95,99,104,101,99,107,95,100,114,97,103,95,104,97,110,100,108,101,100,0,101,118,101,110,116,95,113,117,101,117,101,95,99,111,110,115,101,99,117,116,105,118,101,95,103,101,115,116,117,114,101,95,116,121,112,101,0,101,118,101,110,116,
95,113,117,101,117,101,95,99,111,110,115,101,99,117,116,105,118,101,95,103,101,115,116,117,114,101,95,120,121,91,50,93,0,42,101,118,101,110,116,95,113,117,101,117,101,95,99,111,110,115,101,99,117,116,105,118,101,95,103,101,115,116,117,114,101,95,100,97,116,97,0,42,101,118,101,110,116,115,116,97,116,101,0,42,101,118,101,110,116,95,108,97,115,116,95,104,97,110,100,108,101,100,0,97,100,100,109,111,117,115,101,109,111,118,101,0,109,111,100,97,
108,104,97,110,100,108,101,114,115,0,103,101,115,116,117,114,101,0,100,114,97,119,99,97,108,108,115,0,42,99,117,114,115,111,114,95,107,101,121,109,97,112,95,115,116,97,116,117,115,0,101,118,101,110,116,115,116,97,116,101,95,112,114,101,118,95,112,114,101,115,115,95,116,105,109,101,95,109,115,0,112,114,111,112,118,97,108,117,101,95,115,116,114,91,54,52,93,0,112,114,111,112,118,97,108,117,101,0,115,104,105,102,116,0,99,116,114,108,0,97,
108,116,0,111,115,107,101,121,0,104,121,112,101,114,0,107,101,121,109,111,100,105,102,105,101,114,0,109,97,112,116,121,112,101,0,42,112,116,114,0,42,114,101,109,111,118,101,95,105,116,101,109,0,42,97,100,100,95,105,116,101,109,0,100,105,102,102,95,105,116,101,109,115,0,115,112,97,99,101,105,100,0,114,101,103,105,111,110,105,100,0,111,119,110,101,114,95,105,100,91,49,50,56,93,0,107,109,105,95,105,100,0,40,42,112,111,108,108,41,40,
41,0,40,42,112,111,108,108,95,109,111,100,97,108,95,105,116,101,109,41,40,41,0,42,109,111,100,97,108,95,105,116,101,109,115,0,98,97,115,101,110,97,109,101,91,54,52,93,0,107,101,121,109,97,112,115,0,97,99,116,107,101,121,109,97,112,0,42,99,117,115,116,111,109,100,97,116,97,0,42,114,101,112,111,114,116,115,0,109,97,99,114,111,0,42,111,112,109,0,105,100,110,97,109,101,95,102,97,108,108,98,97,99,107,91,54,52,93,0,105,
100,110,97,109,101,95,112,101,110,100,105,110,103,91,54,52,93,0,108,97,121,111,117,116,115,0,104,111,111,107,95,108,97,121,111,117,116,95,114,101,108,97,116,105,111,110,115,0,111,119,110,101,114,95,105,100,115,0,116,111,111,108,115,0,42,112,105,110,95,115,99,101,110,101,0,111,98,106,101,99,116,95,109,111,100,101,0,111,114,100,101,114,0,112,97,114,101,110,116,105,100,0,95,112,97,100,95,48,91,52,93,0,42,97,99,116,105,118,101,0,
42,97,99,116,95,108,97,121,111,117,116,0,42,116,101,109,112,95,119,111,114,107,115,112,97,99,101,95,115,116,111,114,101,0,42,116,101,109,112,95,108,97,121,111,117,116,95,115,116,111,114,101,0,109,105,115,116,121,112,101,0,104,111,114,114,0,104,111,114,103,0,104,111,114,98,0,101,120,112,0,109,105,115,105,0,109,105,115,116,115,116,97,0,109,105,115,116,100,105,115,116,0,109,105,115,116,104,105,0,97,111,100,105,115,116,0,97,111,101,110,
101,114,103,121,0,112,114,111,98,101,95,114,101,115,111,108,117,116,105,111,110,0,115,117,110,95,116,104,114,101,115,104,111,108,100,0,115,117,110,95,115,104,97,100,111,119,95,109,97,120,105,109,117,109,95,114,101,115,111,108,117,116,105,111,110,0,115,117,110,95,115,104,97,100,111,119,95,106,105,116,116,101,114,95,111,118,101,114,98,108,117,114,0,115,117,110,95,115,104,97,100,111,119,95,102,105,108,116,101,114,95,114,97,100,105,117,115,0,98,97,
115,101,95,115,99,97,108,101,0,98,97,115,101,95,112,111,115,101,95,116,121,112,101,0,42,98,97,115,101,95,112,111,115,101,95,111,98,106,101,99,116,0,98,97,115,101,95,112,111,115,101,95,108,111,99,97,116,105,111,110,91,51,93,0,98,97,115,101,95,112,111,115,101,95,97,110,103,108,101,0,100,114,97,119,95,102,108,97,103,115,0,99,111,110,116,114,111,108,108,101,114,95,100,114,97,119,95,115,116,121,108,101,0,99,108,105,112,95,115,116,
97,114,116,0,99,108,105,112,95,101,110,100,0,112,97,116,104,91,49,57,50,93,0,112,114,111,102,105,108,101,91,50,53,54,93,0,99,111,109,112,111,110,101,110,116,95,112,97,116,104,115,0,102,108,111,97,116,95,116,104,114,101,115,104,111,108,100,0,97,120,105,115,95,102,108,97,103,0,112,111,115,101,95,108,111,99,97,116,105,111,110,91,51,93,0,112,111,115,101,95,114,111,116,97,116,105,111,110,91,51,93,0,112,97,116,104,91,54,52,93,
0,117,115,101,114,95,112,97,116,104,115,0,111,112,91,54,52,93,0,42,111,112,95,112,114,111,112,101,114,116,105,101,115,0,42,111,112,95,112,114,111,112,101,114,116,105,101,115,95,112,116,114,0,111,112,95,102,108,97,103,0,97,99,116,105,111,110,95,102,108,97,103,0,104,97,112,116,105,99,95,102,108,97,103,0,112,111,115,101,95,102,108,97,103,0,104,97,112,116,105,99,95,110,97,109,101,91,54,52,93,0,104,97,112,116,105,99,95,100,117,
114,97,116,105,111,110,0,104,97,112,116,105,99,95,102,114,101,113,117,101,110,99,121,0,104,97,112,116,105,99,95,97,109,112,108,105,116,117,100,101,0,115,101,108,98,105,110,100,105,110,103,0,98,105,110,100,105,110,103,115,0,115,101,108,105,116,101,109,0,0,0,0,84,89,80,69,71,4,0,0,99,104,97,114,0,117,99,104,97,114,0,115,104,111,114,116,0,117,115,104,111,114,116,0,105,110,116,0,108,111,110,103,0,117,108,111,110,103,0,102,
108,111,97,116,0,100,111,117,98,108,101,0,105,110,116,54,52,95,116,0,117,105,110,116,54,52,95,116,0,118,111,105,100,0,105,110,116,56,95,116,0,114,97,119,95,100,97,116,97,0,73,68,80,114,111,112,101,114,116,121,85,73,68,97,116,97,0,73,68,80,114,111,112,101,114,116,121,85,73,68,97,116,97,69,110,117,109,73,116,101,109,0,73,68,80,114,111,112,101,114,116,121,85,73,68,97,116,97,73,110,116,0,73,68,80,114,111,112,101,114,
116,121,85,73,68,97,116,97,66,111,111,108,0,73,68,80,114,111,112,101,114,116,121,85,73,68,97,116,97,70,108,111,97,116,0,73,68,80,114,111,112,101,114,116,121,85,73,68,97,116,97,83,116,114,105,110,103,0,73,68,80,114,111,112,101,114,116,121,85,73,68,97,116,97,73,68,0,73,68,80,114,111,112,101,114,116,121,68,97,116,97,0,76,105,115,116,66,97,115,101,0,73,68,80,114,111,112,101,114,116,121,0,73,68,79,118,101,114,114,105,
100,101,76,105,98,114,97,114,121,80,114,111,112,101,114,116,121,79,112,101,114,97,116,105,111,110,0,73,68,0,73,68,79,118,101,114,114,105,100,101,76,105,98,114,97,114,121,80,114,111,112,101,114,116,121,0,73,68,79,118,101,114,114,105,100,101,76,105,98,114,97,114,121,0,73,68,79,118,101,114,114,105,100,101,76,105,98,114,97,114,121,82,117,110,116,105,109,101,0,73,68,95,82,117,110,116,105,109,101,95,82,101,109,97,112,0,73,68,95,82,
117,110,116,105,109,101,0,68,101,112,115,103,114,97,112,104,0,73,68,95,82,101,97,100,102,105,108,101,95,68,97,116,97,0,76,105,98,114,97,114,121,0,65,115,115,101,116,77,101,116,97,68,97,116,97,0,76,105,98,114,97,114,121,87,101,97,107,82,101,102,101,114,101,110,99,101,0,80,97,99,107,101,100,70,105,108,101,0,76,105,98,114,97,114,121,82,117,110,116,105,109,101,72,97,110,100,108,101,0,80,114,101,118,105,101,119,73,109,97,103,
101,0,80,114,101,118,105,101,119,73,109,97,103,101,82,117,110,116,105,109,101,72,97,110,100,108,101,0,98,77,111,116,105,111,110,80,97,116,104,86,101,114,116,0,98,77,111,116,105,111,110,80,97,116,104,0,71,80,85,86,101,114,116,66,117,102,72,97,110,100,108,101,0,71,80,85,66,97,116,99,104,72,97,110,100,108,101,0,98,65,110,105,109,86,105,122,83,101,116,116,105,110,103,115,0,98,80,111,115,101,67,104,97,110,110,101,108,95,66,66,
111,110,101,83,101,103,109,101,110,116,66,111,117,110,100,97,114,121,0,98,80,111,115,101,67,104,97,110,110,101,108,95,82,117,110,116,105,109,101,0,83,101,115,115,105,111,110,85,73,68,0,68,117,97,108,81,117,97,116,0,77,97,116,52,0,98,80,111,115,101,67,104,97,110,110,101,108,0,66,111,110,101,0,79,98,106,101,99,116,0,98,80,111,115,101,67,104,97,110,110,101,108,68,114,97,119,68,97,116,97,0,66,111,110,101,67,111,108,111,114,
0,98,80,111,115,101,0,71,72,97,115,104,0,98,73,75,80,97,114,97,109,0,98,73,116,97,115,99,0,98,65,99,116,105,111,110,71,114,111,117,112,0,65,99,116,105,111,110,67,104,97,110,110,101,108,66,97,103,0,84,104,101,109,101,87,105,114,101,67,111,108,111,114,0,98,65,99,116,105,111,110,0,65,99,116,105,111,110,76,97,121,101,114,0,65,99,116,105,111,110,83,108,111,116,0,65,99,116,105,111,110,83,116,114,105,112,75,101,121,102,
114,97,109,101,68,97,116,97,0,98,68,111,112,101,83,104,101,101,116,0,67,111,108,108,101,99,116,105,111,110,0,83,112,97,99,101,65,99,116,105,111,110,95,82,117,110,116,105,109,101,0,83,112,97,99,101,65,99,116,105,111,110,0,83,112,97,99,101,76,105,110,107,0,86,105,101,119,50,68,0,98,65,99,116,105,111,110,67,104,97,110,110,101,108,0,73,112,111,0,65,99,116,105,111,110,83,116,114,105,112,0,65,99,116,105,111,110,83,108,111,
116,82,117,110,116,105,109,101,72,97,110,100,108,101,0,70,67,117,114,118,101,0,70,77,111,100,105,102,105,101,114,0,70,77,111,100,95,71,101,110,101,114,97,116,111,114,0,70,77,111,100,95,70,117,110,99,116,105,111,110,71,101,110,101,114,97,116,111,114,0,70,67,77,95,69,110,118,101,108,111,112,101,68,97,116,97,0,70,77,111,100,95,69,110,118,101,108,111,112,101,0,70,77,111,100,95,67,121,99,108,101,115,0,70,77,111,100,95,76,105,
109,105,116,115,0,114,99,116,102,0,70,77,111,100,95,78,111,105,115,101,0,70,77,111,100,95,83,116,101,112,112,101,100,0,68,114,105,118,101,114,84,97,114,103,101,116,0,68,114,105,118,101,114,86,97,114,0,67,104,97,110,110,101,108,68,114,105,118,101,114,0,69,120,112,114,80,121,76,105,107,101,95,80,97,114,115,101,100,0,70,80,111,105,110,116,0,66,101,122,84,114,105,112,108,101,0,78,108,97,83,116,114,105,112,0,78,108,97,84,114,
97,99,107,0,75,83,95,80,97,116,104,0,75,101,121,105,110,103,83,101,116,0,65,110,105,109,79,118,101,114,114,105,100,101,0,65,110,105,109,68,97,116,97,0,73,100,65,100,116,84,101,109,112,108,97,116,101,0,66,111,110,101,95,82,117,110,116,105,109,101,0,98,65,114,109,97,116,117,114,101,95,82,117,110,116,105,109,101,0,66,111,110,101,67,111,108,108,101,99,116,105,111,110,0,98,65,114,109,97,116,117,114,101,0,69,100,105,116,66,111,
110,101,0,66,111,110,101,67,111,108,108,101,99,116,105,111,110,77,101,109,98,101,114,0,66,111,110,101,67,111,108,108,101,99,116,105,111,110,82,101,102,101,114,101,110,99,101,0,65,115,115,101,116,84,97,103,0,65,115,115,101,116,84,121,112,101,73,110,102,111,0,98,85,85,73,68,0,65,115,115,101,116,76,105,98,114,97,114,121,82,101,102,101,114,101,110,99,101,0,65,115,115,101,116,87,101,97,107,82,101,102,101,114,101,110,99,101,0,65,115,
115,101,116,67,97,116,97,108,111,103,80,97,116,104,76,105,110,107,0,65,116,116,114,105,98,117,116,101,65,114,114,97,121,0,73,109,112,108,105,99,105,116,83,104,97,114,105,110,103,73,110,102,111,72,97,110,100,108,101,0,65,116,116,114,105,98,117,116,101,83,105,110,103,108,101,0,65,116,116,114,105,98,117,116,101,0,65,116,116,114,105,98,117,116,101,83,116,111,114,97,103,101,0,65,116,116,114,105,98,117,116,101,83,116,111,114,97,103,101,82,
117,110,116,105,109,101,72,97,110,100,108,101,0,66,111,105,100,82,117,108,101,0,66,111,105,100,82,117,108,101,71,111,97,108,65,118,111,105,100,0,66,111,105,100,82,117,108,101,65,118,111,105,100,67,111,108,108,105,115,105,111,110,0,66,111,105,100,82,117,108,101,70,111,108,108,111,119,76,101,97,100,101,114,0,66,111,105,100,82,117,108,101,65,118,101,114,97,103,101,83,112,101,101,100,0,66,111,105,100,82,117,108,101,70,105,103,104,116,0,66,
111,105,100,68,97,116,97,0,66,111,105,100,83,116,97,116,101,0,66,111,105,100,83,101,116,116,105,110,103,115,0,66,114,117,115,104,71,112,101,110,99,105,108,83,101,116,116,105,110,103,115,0,67,117,114,118,101,77,97,112,112,105,110,103,0,77,97,116,101,114,105,97,108,0,66,114,117,115,104,67,117,114,118,101,115,83,99,117,108,112,116,83,101,116,116,105,110,103,115,0,66,114,117,115,104,0,77,84,101,120,0,73,109,66,117,102,0,67,111,108,
111,114,66,97,110,100,0,80,97,105,110,116,67,117,114,118,101,0,116,80,97,108,101,116,116,101,67,111,108,111,114,72,83,86,0,80,97,108,101,116,116,101,67,111,108,111,114,0,80,97,108,101,116,116,101,0,80,97,105,110,116,67,117,114,118,101,80,111,105,110,116,0,67,97,99,104,101,79,98,106,101,99,116,80,97,116,104,0,67,97,99,104,101,70,105,108,101,76,97,121,101,114,0,67,97,99,104,101,70,105,108,101,0,67,97,99,104,101,65,114,
99,104,105,118,101,72,97,110,100,108,101,0,71,83,101,116,0,67,97,109,101,114,97,83,116,101,114,101,111,83,101,116,116,105,110,103,115,0,67,97,109,101,114,97,66,71,73,109,97,103,101,0,73,109,97,103,101,0,73,109,97,103,101,85,115,101,114,0,77,111,118,105,101,67,108,105,112,0,77,111,118,105,101,67,108,105,112,85,115,101,114,0,67,97,109,101,114,97,68,79,70,83,101,116,116,105,110,103,115,0,67,97,109,101,114,97,95,82,117,110,
116,105,109,101,0,67,97,109,101,114,97,0,84,101,120,116,0,71,80,85,68,79,70,83,101,116,116,105,110,103,115,0,67,108,111,116,104,83,105,109,83,101,116,116,105,110,103,115,0,76,105,110,107,78,111,100,101,0,69,102,102,101,99,116,111,114,87,101,105,103,104,116,115,0,67,108,111,116,104,67,111,108,108,83,101,116,116,105,110,103,115,0,67,111,108,108,101,99,116,105,111,110,76,105,103,104,116,76,105,110,107,105,110,103,0,67,111,108,108,101,
99,116,105,111,110,79,98,106,101,99,116,0,67,111,108,108,101,99,116,105,111,110,67,104,105,108,100,0,67,111,108,108,101,99,116,105,111,110,69,120,112,111,114,116,0,67,111,108,108,101,99,116,105,111,110,95,82,117,110,116,105,109,101,0,86,105,101,119,76,97,121,101,114,0,67,117,114,118,101,77,97,112,80,111,105,110,116,0,67,117,114,118,101,77,97,112,0,72,105,115,116,111,103,114,97,109,0,83,99,111,112,101,115,0,67,111,108,111,114,77,
97,110,97,103,101,100,86,105,101,119,83,101,116,116,105,110,103,115,0,67,111,108,111,114,77,97,110,97,103,101,100,68,105,115,112,108,97,121,83,101,116,116,105,110,103,115,0,67,111,108,111,114,77,97,110,97,103,101,100,67,111,108,111,114,115,112,97,99,101,83,101,116,116,105,110,103,115,0,67,66,68,97,116,97,0,98,67,111,110,115,116,114,97,105,110,116,67,104,97,110,110,101,108,0,98,67,111,110,115,116,114,97,105,110,116,0,98,67,111,110,
115,116,114,97,105,110,116,84,97,114,103,101,116,0,98,75,105,110,101,109,97,116,105,99,67,111,110,115,116,114,97,105,110,116,0,98,83,112,108,105,110,101,73,75,67,111,110,115,116,114,97,105,110,116,0,98,65,114,109,97,116,117,114,101,67,111,110,115,116,114,97,105,110,116,0,98,84,114,97,99,107,84,111,67,111,110,115,116,114,97,105,110,116,0,98,82,111,116,97,116,101,76,105,107,101,67,111,110,115,116,114,97,105,110,116,0,98,76,111,99,
97,116,101,76,105,107,101,67,111,110,115,116,114,97,105,110,116,0,98,83,105,122,101,76,105,107,101,67,111,110,115,116,114,97,105,110,116,0,98,83,97,109,101,86,111,108,117,109,101,67,111,110,115,116,114,97,105,110,116,0,98,84,114,97,110,115,76,105,107,101,67,111,110,115,116,114,97,105,110,116,0,98,77,105,110,77,97,120,67,111,110,115,116,114,97,105,110,116,0,98,65,99,116,105,111,110,67,111,110,115,116,114,97,105,110,116,0,98,76,111,
99,107,84,114,97,99,107,67,111,110,115,116,114,97,105,110,116,0,98,68,97,109,112,84,114,97,99,107,67,111,110,115,116,114,97,105,110,116,0,98,70,111,108,108,111,119,80,97,116,104,67,111,110,115,116,114,97,105,110,116,0,98,83,116,114,101,116,99,104,84,111,67,111,110,115,116,114,97,105,110,116,0,98,82,105,103,105,100,66,111,100,121,74,111,105,110,116,67,111,110,115,116,114,97,105,110,116,0,98,67,108,97,109,112,84,111,67,111,110,115,
116,114,97,105,110,116,0,98,67,104,105,108,100,79,102,67,111,110,115,116,114,97,105,110,116,0,98,84,114,97,110,115,102,111,114,109,67,111,110,115,116,114,97,105,110,116,0,98,80,105,118,111,116,67,111,110,115,116,114,97,105,110,116,0,98,76,111,99,76,105,109,105,116,67,111,110,115,116,114,97,105,110,116,0,98,82,111,116,76,105,109,105,116,67,111,110,115,116,114,97,105,110,116,0,98,83,105,122,101,76,105,109,105,116,67,111,110,115,116,114,
97,105,110,116,0,98,68,105,115,116,76,105,109,105,116,67,111,110,115,116,114,97,105,110,116,0,98,83,104,114,105,110,107,119,114,97,112,67,111,110,115,116,114,97,105,110,116,0,98,70,111,108,108,111,119,84,114,97,99,107,67,111,110,115,116,114,97,105,110,116,0,98,67,97,109,101,114,97,83,111,108,118,101,114,67,111,110,115,116,114,97,105,110,116,0,98,79,98,106,101,99,116,83,111,108,118,101,114,67,111,110,115,116,114,97,105,110,116,0,98,
84,114,97,110,115,102,111,114,109,67,97,99,104,101,67,111,110,115,116,114,97,105,110,116,0,67,97,99,104,101,82,101,97,100,101,114,0,66,80,111,105,110,116,0,78,117,114,98,0,67,104,97,114,73,110,102,111,0,84,101,120,116,66,111,120,0,67,117,114,118,101,0,69,100,105,116,78,117,114,98,0,75,101,121,0,67,117,114,118,101,80,114,111,102,105,108,101,0,69,100,105,116,70,111,110,116,0,86,70,111,110,116,0,67,117,114,118,101,115,0,
67,117,114,118,101,80,114,111,102,105,108,101,80,111,105,110,116,0,67,117,114,118,101,115,71,101,111,109,101,116,114,121,0,67,117,115,116,111,109,68,97,116,97,0,67,117,114,118,101,115,71,101,111,109,101,116,114,121,82,117,110,116,105,109,101,72,97,110,100,108,101,0,67,117,115,116,111,109,68,97,116,97,76,97,121,101,114,0,67,117,115,116,111,109,68,97,116,97,69,120,116,101,114,110,97,108,0,66,76,73,95,109,101,109,112,111,111,108,0,67,
117,115,116,111,109,68,97,116,97,95,77,101,115,104,77,97,115,107,115,0,68,121,110,97,109,105,99,80,97,105,110,116,83,117,114,102,97,99,101,0,68,121,110,97,109,105,99,80,97,105,110,116,67,97,110,118,97,115,83,101,116,116,105,110,103,115,0,80,97,105,110,116,83,117,114,102,97,99,101,68,97,116,97,0,80,111,105,110,116,67,97,99,104,101,0,84,101,120,0,68,121,110,97,109,105,99,80,97,105,110,116,77,111,100,105,102,105,101,114,68,
97,116,97,0,68,121,110,97,109,105,99,80,97,105,110,116,66,114,117,115,104,83,101,116,116,105,110,103,115,0,80,97,114,116,105,99,108,101,83,121,115,116,101,109,0,69,102,102,101,99,116,0,66,117,105,108,100,69,102,102,0,80,97,114,116,69,102,102,0,80,97,114,116,105,99,108,101,0,87,97,118,101,69,102,102,0,70,105,108,101,71,108,111,98,97,108,0,98,83,99,114,101,101,110,0,83,99,101,110,101,0,70,108,117,105,100,68,111,109,97,
105,110,83,101,116,116,105,110,103,115,0,70,108,117,105,100,77,111,100,105,102,105,101,114,68,97,116,97,0,77,65,78,84,65,0,71,80,85,84,101,120,116,117,114,101,0,70,108,117,105,100,70,108,111,119,83,101,116,116,105,110,103,115,0,77,101,115,104,0,70,108,117,105,100,69,102,102,101,99,116,111,114,83,101,116,116,105,110,103,115,0,70,114,101,101,115,116,121,108,101,76,105,110,101,83,101,116,0,70,114,101,101,115,116,121,108,101,76,105,110,
101,83,116,121,108,101,0,70,114,101,101,115,116,121,108,101,77,111,100,117,108,101,67,111,110,102,105,103,0,70,114,101,101,115,116,121,108,101,67,111,110,102,105,103,0,98,71,80,68,115,112,111,105,110,116,0,98,71,80,68,116,114,105,97,110,103,108,101,0,98,71,80,68,112,97,108,101,116,116,101,99,111,108,111,114,0,98,71,80,68,112,97,108,101,116,116,101,0,98,71,80,68,99,117,114,118,101,95,112,111,105,110,116,0,98,71,80,68,99,117,
114,118,101,0,98,71,80,68,115,116,114,111,107,101,95,82,117,110,116,105,109,101,0,98,71,80,68,115,116,114,111,107,101,0,77,68,101,102,111,114,109,86,101,114,116,0,98,71,80,68,102,114,97,109,101,95,82,117,110,116,105,109,101,0,98,71,80,68,102,114,97,109,101,0,98,71,80,68,108,97,121,101,114,95,77,97,115,107,0,98,71,80,68,108,97,121,101,114,95,82,117,110,116,105,109,101,0,98,71,80,68,108,97,121,101,114,0,98,71,80,
100,97,116,97,95,82,117,110,116,105,109,101,0,98,71,80,103,114,105,100,0,98,71,80,100,97,116,97,0,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,68,97,116,97,0,78,111,105,115,101,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,68,97,116,97,0,83,117,98,100,105,118,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,68,97,116,97,0,84,104,105,99,107,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,
68,97,116,97,0,84,105,109,101,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,83,101,103,109,101,110,116,0,84,105,109,101,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,68,97,116,97,0,67,111,108,111,114,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,68,97,116,97,0,79,112,97,99,105,116,121,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,68,97,116,97,0,79,117,116,108,105,110,101,71,112,101,110,99,
105,108,77,111,100,105,102,105,101,114,68,97,116,97,0,65,114,114,97,121,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,68,97,116,97,0,66,117,105,108,100,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,68,97,116,97,0,76,97,116,116,105,99,101,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,68,97,116,97,0,76,97,116,116,105,99,101,68,101,102,111,114,109,68,97,116,97,0,76,101,110,103,116,104,71,112,101,110,
99,105,108,77,111,100,105,102,105,101,114,68,97,116,97,0,68,97,115,104,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,83,101,103,109,101,110,116,0,68,97,115,104,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,68,97,116,97,0,77,105,114,114,111,114,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,68,97,116,97,0,72,111,111,107,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,68,97,116,97,0,83,105,109,
112,108,105,102,121,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,68,97,116,97,0,79,102,102,115,101,116,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,68,97,116,97,0,83,109,111,111,116,104,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,68,97,116,97,0,65,114,109,97,116,117,114,101,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,68,97,116,97,0,77,117,108,116,105,112,108,121,71,112,101,110,99,105,108,
77,111,100,105,102,105,101,114,68,97,116,97,0,84,105,110,116,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,68,97,116,97,0,84,101,120,116,117,114,101,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,68,97,116,97,0,87,101,105,103,104,116,80,114,111,120,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,68,97,116,97,0,87,101,105,103,104,116,65,110,103,108,101,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,
68,97,116,97,0,76,105,110,101,97,114,116,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,68,97,116,97,0,76,105,110,101,97,114,116,67,97,99,104,101,0,76,105,110,101,97,114,116,68,97,116,97,0,83,104,114,105,110,107,119,114,97,112,71,112,101,110,99,105,108,77,111,100,105,102,105,101,114,68,97,116,97,0,83,104,114,105,110,107,119,114,97,112,84,114,101,101,68,97,116,97,0,69,110,118,101,108,111,112,101,71,112,101,110,99,105,
108,77,111,100,105,102,105,101,114,68,97,116,97,0,71,114,101,97,115,101,80,101,110,99,105,108,68,114,97,119,105,110,103,66,97,115,101,0,71,114,101,97,115,101,80,101,110,99,105,108,68,114,97,119,105,110,103,0,71,114,101,97,115,101,80,101,110,99,105,108,68,114,97,119,105,110,103,82,117,110,116,105,109,101,72,97,110,100,108,101,0,71,114,101,97,115,101,80,101,110,99,105,108,68,114,97,119,105,110,103,82,101,102,101,114,101,110,99,101,0,
71,114,101,97,115,101,80,101,110,99,105,108,0,71,114,101,97,115,101,80,101,110,99,105,108,70,114,97,109,101,0,71,114,101,97,115,101,80,101,110,99,105,108,76,97,121,101,114,70,114,97,109,101,115,77,97,112,83,116,111,114,97,103,101,0,71,114,101,97,115,101,80,101,110,99,105,108,76,97,121,101,114,77,97,115,107,0,71,114,101,97,115,101,80,101,110,99,105,108,76,97,121,101,114,84,114,101,101,78,111,100,101,0,71,114,101,97,115,101,80,
101,110,99,105,108,76,97,121,101,114,84,114,101,101,71,114,111,117,112,0,71,114,101,97,115,101,80,101,110,99,105,108,76,97,121,101,114,0,71,114,101,97,115,101,80,101,110,99,105,108,76,97,121,101,114,82,117,110,116,105,109,101,72,97,110,100,108,101,0,71,114,101,97,115,101,80,101,110,99,105,108,76,97,121,101,114,71,114,111,117,112,82,117,110,116,105,109,101,72,97,110,100,108,101,0,71,114,101,97,115,101,80,101,110,99,105,108,79,110,105,
111,110,83,107,105,110,110,105,110,103,83,101,116,116,105,110,103,115,0,71,114,101,97,115,101,80,101,110,99,105,108,82,117,110,116,105,109,101,72,97,110,100,108,101,0,73,109,97,103,101,65,110,105,109,0,77,111,118,105,101,82,101,97,100,101,114,0,73,109,97,103,101,86,105,101,119,0,73,109,97,103,101,80,97,99,107,101,100,70,105,108,101,0,82,101,110,100,101,114,83,108,111,116,0,82,101,110,100,101,114,82,101,115,117,108,116,0,73,109,97,
103,101,84,105,108,101,95,82,117,110,116,105,109,101,0,73,109,97,103,101,84,105,108,101,0,77,111,118,105,101,67,97,99,104,101,0,83,116,101,114,101,111,51,100,70,111,114,109,97,116,0,73,109,97,103,101,82,117,110,116,105,109,101,72,97,110,100,108,101,0,73,112,111,68,114,105,118,101,114,0,73,112,111,67,117,114,118,101,0,75,101,121,66,108,111,99,107,0,76,97,116,116,105,99,101,0,69,100,105,116,76,97,116,116,0,66,97,115,101,0,
76,97,121,101,114,67,111,108,108,101,99,116,105,111,110,0,86,105,101,119,76,97,121,101,114,69,69,86,69,69,0,86,105,101,119,76,97,121,101,114,65,79,86,0,86,105,101,119,76,97,121,101,114,76,105,103,104,116,103,114,111,117,112,0,76,105,103,104,116,103,114,111,117,112,77,101,109,98,101,114,115,104,105,112,0,83,99,101,110,101,83,116,97,116,115,0,87,111,114,108,100,0,76,97,109,112,0,98,78,111,100,101,84,114,101,101,0,76,105,103,
104,116,80,114,111,98,101,0,76,105,103,104,116,80,114,111,98,101,67,97,99,104,101,0,76,105,103,104,116,71,114,105,100,67,97,99,104,101,0,76,105,103,104,116,67,97,99,104,101,84,101,120,116,117,114,101,0,76,105,103,104,116,67,97,99,104,101,0,76,105,103,104,116,80,114,111,98,101,66,97,107,105,110,103,68,97,116,97,0,76,105,103,104,116,80,114,111,98,101,73,114,114,97,100,105,97,110,99,101,68,97,116,97,0,76,105,103,104,116,80,
114,111,98,101,86,105,115,105,98,105,108,105,116,121,68,97,116,97,0,76,105,103,104,116,80,114,111,98,101,67,111,110,110,101,99,116,105,118,105,116,121,68,97,116,97,0,76,105,103,104,116,80,114,111,98,101,66,108,111,99,107,68,97,116,97,0,76,105,103,104,116,80,114,111,98,101,71,114,105,100,67,97,99,104,101,70,114,97,109,101,0,76,105,103,104,116,80,114,111,98,101,79,98,106,101,99,116,67,97,99,104,101,0,76,105,110,101,83,116,121,
108,101,77,111,100,105,102,105,101,114,0,76,105,110,101,83,116,121,108,101,67,111,108,111,114,77,111,100,105,102,105,101,114,95,65,108,111,110,103,83,116,114,111,107,101,0,76,105,110,101,83,116,121,108,101,65,108,112,104,97,77,111,100,105,102,105,101,114,95,65,108,111,110,103,83,116,114,111,107,101,0,76,105,110,101,83,116,121,108,101,84,104,105,99,107,110,101,115,115,77,111,100,105,102,105,101,114,95,65,108,111,110,103,83,116,114,111,107,101,0,
76,105,110,101,83,116,121,108,101,67,111,108,111,114,77,111,100,105,102,105,101,114,95,68,105,115,116,97,110,99,101,70,114,111,109,67,97,109,101,114,97,0,76,105,110,101,83,116,121,108,101,65,108,112,104,97,77,111,100,105,102,105,101,114,95,68,105,115,116,97,110,99,101,70,114,111,109,67,97,109,101,114,97,0,76,105,110,101,83,116,121,108,101,84,104,105,99,107,110,101,115,115,77,111,100,105,102,105,101,114,95,68,105,115,116,97,110,99,101,70,
114,111,109,67,97,109,101,114,97,0,76,105,110,101,83,116,121,108,101,67,111,108,111,114,77,111,100,105,102,105,101,114,95,68,105,115,116,97,110,99,101,70,114,111,109,79,98,106,101,99,116,0,76,105,110,101,83,116,121,108,101,65,108,112,104,97,77,111,100,105,102,105,101,114,95,68,105,115,116,97,110,99,101,70,114,111,109,79,98,106,101,99,116,0,76,105,110,101,83,116,121,108,101,84,104,105,99,107,110,101,115,115,77,111,100,105,102,105,101,114,
95,68,105,115,116,97,110,99,101,70,114,111,109,79,98,106,101,99,116,0,76,105,110,101,83,116,121,108,101,67,111,108,111,114,77,111,100,105,102,105,101,114,95,67,117,114,118,97,116,117,114,101,95,51,68,0,76,105,110,101,83,116,121,108,101,65,108,112,104,97,77,111,100,105,102,105,101,114,95,67,117,114,118,97,116,117,114,101,95,51,68,0,76,105,110,101,83,116,121,108,101,84,104,105,99,107,110,101,115,115,77,111,100,105,102,105,101,114,95,67,
117,114,118,97,116,117,114,101,95,51,68,0,76,105,110,101,83,116,121,108,101,67,111,108,111,114,77,111,100,105,102,105,101,114,95,78,111,105,115,101,0,76,105,110,101,83,116,121,108,101,65,108,112,104,97,77,111,100,105,102,105,101,114,95,78,111,105,115,101,0,76,105,110,101,83,116,121,108,101,84,104,105,99,107,110,101,115,115,77,111,100,105,102,105,101,114,95,78,111,105,115,101,0,76,105,110,101,83,116,121,108,101,67,111,108,111,114,77,111,100,
105,102,105,101,114,95,67,114,101,97,115,101,65,110,103,108,101,0,76,105,110,101,83,116,121,108,101,65,108,112,104,97,77,111,100,105,102,105,101,114,95,67,114,101,97,115,101,65,110,103,108,101,0,76,105,110,101,83,116,121,108,101,84,104,105,99,107,110,101,115,115,77,111,100,105,102,105,101,114,95,67,114,101,97,115,101,65,110,103,108,101,0,76,105,110,101,83,116,121,108,101,67,111,108,111,114,77,111,100,105,102,105,101,114,95,84,97,110,103,101,
110,116,0,76,105,110,101,83,116,121,108,101,65,108,112,104,97,77,111,100,105,102,105,101,114,95,84,97,110,103,101,110,116,0,76,105,110,101,83,116,121,108,101,84,104,105,99,107,110,101,115,115,77,111,100,105,102,105,101,114,95,84,97,110,103,101,110,116,0,76,105,110,101,83,116,121,108,101,67,111,108,111,114,77,111,100,105,102,105,101,114,95,77,97,116,101,114,105,97,108,0,76,105,110,101,83,116,121,108,101,65,108,112,104,97,77,111,100,105,102,
105,101,114,95,77,97,116,101,114,105,97,108,0,76,105,110,101,83,116,121,108,101,84,104,105,99,107,110,101,115,115,77,111,100,105,102,105,101,114,95,77,97,116,101,114,105,97,108,0,76,105,110,101,83,116,121,108,101,71,101,111,109,101,116,114,121,77,111,100,105,102,105,101,114,95,83,97,109,112,108,105,110,103,0,76,105,110,101,83,116,121,108,101,71,101,111,109,101,116,114,121,77,111,100,105,102,105,101,114,95,66,101,122,105,101,114,67,117,114,118,
101,0,76,105,110,101,83,116,121,108,101,71,101,111,109,101,116,114,121,77,111,100,105,102,105,101,114,95,83,105,110,117,115,68,105,115,112,108,97,99,101,109,101,110,116,0,76,105,110,101,83,116,121,108,101,71,101,111,109,101,116,114,121,77,111,100,105,102,105,101,114,95,83,112,97,116,105,97,108,78,111,105,115,101,0,76,105,110,101,83,116,121,108,101,71,101,111,109,101,116,114,121,77,111,100,105,102,105,101,114,95,80,101,114,108,105,110,78,111,105,
115,101,49,68,0,76,105,110,101,83,116,121,108,101,71,101,111,109,101,116,114,121,77,111,100,105,102,105,101,114,95,80,101,114,108,105,110,78,111,105,115,101,50,68,0,76,105,110,101,83,116,121,108,101,71,101,111,109,101,116,114,121,77,111,100,105,102,105,101,114,95,66,97,99,107,98,111,110,101,83,116,114,101,116,99,104,101,114,0,76,105,110,101,83,116,121,108,101,71,101,111,109,101,116,114,121,77,111,100,105,102,105,101,114,95,84,105,112,82,101,
109,111,118,101,114,0,76,105,110,101,83,116,121,108,101,71,101,111,109,101,116,114,121,77,111,100,105,102,105,101,114,95,80,111,108,121,103,111,110,97,108,105,122,97,116,105,111,110,0,76,105,110,101,83,116,121,108,101,71,101,111,109,101,116,114,121,77,111,100,105,102,105,101,114,95,71,117,105,100,105,110,103,76,105,110,101,115,0,76,105,110,101,83,116,121,108,101,71,101,111,109,101,116,114,121,77,111,100,105,102,105,101,114,95,66,108,117,101,112,114,
105,110,116,0,76,105,110,101,83,116,121,108,101,71,101,111,109,101,116,114,121,77,111,100,105,102,105,101,114,95,50,68,79,102,102,115,101,116,0,76,105,110,101,83,116,121,108,101,71,101,111,109,101,116,114,121,77,111,100,105,102,105,101,114,95,50,68,84,114,97,110,115,102,111,114,109,0,76,105,110,101,83,116,121,108,101,71,101,111,109,101,116,114,121,77,111,100,105,102,105,101,114,95,83,105,109,112,108,105,102,105,99,97,116,105,111,110,0,76,105,
110,101,83,116,121,108,101,84,104,105,99,107,110,101,115,115,77,111,100,105,102,105,101,114,95,67,97,108,108,105,103,114,97,112,104,121,0,76,105,110,107,0,76,105,110,107,68,97,116,97,0,77,97,115,107,95,82,117,110,116,105,109,101,0,77,97,115,107,0,77,97,115,107,80,97,114,101,110,116,0,77,97,115,107,83,112,108,105,110,101,80,111,105,110,116,85,87,0,77,97,115,107,83,112,108,105,110,101,80,111,105,110,116,0,77,97,115,107,83,112,
108,105,110,101,0,77,97,115,107,76,97,121,101,114,83,104,97,112,101,0,77,97,115,107,76,97,121,101,114,0,84,101,120,80,97,105,110,116,83,108,111,116,0,77,97,116,101,114,105,97,108,71,80,101,110,99,105,108,83,116,121,108,101,0,77,97,116,101,114,105,97,108,76,105,110,101,65,114,116,0,77,83,101,108,101,99,116,0,77,80,111,108,121,0,77,76,111,111,112,0,77,86,101,114,116,0,77,69,100,103,101,0,77,84,70,97,99,101,0,84,
70,97,99,101,0,77,67,111,108,0,77,70,97,99,101,0,77,101,115,104,82,117,110,116,105,109,101,72,97,110,100,108,101,0,77,70,108,111,97,116,80,114,111,112,101,114,116,121,0,77,73,110,116,80,114,111,112,101,114,116,121,0,77,83,116,114,105,110,103,80,114,111,112,101,114,116,121,0,77,66,111,111,108,80,114,111,112,101,114,116,121,0,77,73,110,116,56,80,114,111,112,101,114,116,121,0,77,68,101,102,111,114,109,87,101,105,103,104,116,0,
77,86,101,114,116,83,107,105,110,0,77,76,111,111,112,67,111,108,0,77,80,114,111,112,67,111,108,0,77,68,105,115,112,115,0,71,114,105,100,80,97,105,110,116,77,97,115,107,0,70,114,101,101,115,116,121,108,101,69,100,103,101,0,70,114,101,101,115,116,121,108,101,70,97,99,101,0,77,76,111,111,112,85,86,0,77,82,101,99,97,115,116,0,77,101,116,97,69,108,101,109,0,66,111,117,110,100,66,111,120,0,77,101,116,97,66,97,108,108,0,
77,111,100,105,102,105,101,114,68,97,116,97,0,77,97,112,112,105,110,103,73,110,102,111,77,111,100,105,102,105,101,114,68,97,116,97,0,83,117,98,115,117,114,102,77,111,100,105,102,105,101,114,68,97,116,97,0,76,97,116,116,105,99,101,77,111,100,105,102,105,101,114,68,97,116,97,0,67,117,114,118,101,77,111,100,105,102,105,101,114,68,97,116,97,0,66,117,105,108,100,77,111,100,105,102,105,101,114,68,97,116,97,0,77,97,115,107,77,111,100,
105,102,105,101,114,68,97,116,97,0,65,114,114,97,121,77,111,100,105,102,105,101,114,68,97,116,97,0,77,105,114,114,111,114,77,111,100,105,102,105,101,114,68,97,116,97,0,69,100,103,101,83,112,108,105,116,77,111,100,105,102,105,101,114,68,97,116,97,0,66,101,118,101,108,77,111,100,105,102,105,101,114,68,97,116,97,0,68,105,115,112,108,97,99,101,77,111,100,105,102,105,101,114,68,97,116,97,0,85,86,80,114,111,106,101,99,116,77,111,100,
105,102,105,101,114,68,97,116,97,0,68,101,99,105,109,97,116,101,77,111,100,105,102,105,101,114,68,97,116,97,0,83,109,111,111,116,104,77,111,100,105,102,105,101,114,68,97,116,97,0,67,97,115,116,77,111,100,105,102,105,101,114,68,97,116,97,0,87,97,118,101,77,111,100,105,102,105,101,114,68,97,116,97,0,65,114,109,97,116,117,114,101,77,111,100,105,102,105,101,114,68,97,116,97,0,72,111,111,107,77,111,100,105,102,105,101,114,68,97,116,
97,0,83,111,102,116,98,111,100,121,77,111,100,105,102,105,101,114,68,97,116,97,0,67,108,111,116,104,77,111,100,105,102,105,101,114,68,97,116,97,0,67,108,111,116,104,0,67,108,111,116,104,72,97,105,114,68,97,116,97,0,67,108,111,116,104,83,111,108,118,101,114,82,101,115,117,108,116,0,67,111,108,108,105,115,105,111,110,77,111,100,105,102,105,101,114,68,97,116,97,0,66,86,72,84,114,101,101,0,83,117,114,102,97,99,101,77,111,100,105,
102,105,101,114,68,97,116,97,95,82,117,110,116,105,109,101,0,66,86,72,84,114,101,101,70,114,111,109,77,101,115,104,72,97,110,100,108,101,0,83,117,114,102,97,99,101,77,111,100,105,102,105,101,114,68,97,116,97,0,66,111,111,108,101,97,110,77,111,100,105,102,105,101,114,68,97,116,97,0,77,68,101,102,73,110,102,108,117,101,110,99,101,0,77,68,101,102,67,101,108,108,0,77,101,115,104,68,101,102,111,114,109,77,111,100,105,102,105,101,114,
68,97,116,97,0,80,97,114,116,105,99,108,101,83,121,115,116,101,109,77,111,100,105,102,105,101,114,68,97,116,97,0,80,97,114,116,105,99,108,101,73,110,115,116,97,110,99,101,77,111,100,105,102,105,101,114,68,97,116,97,0,69,120,112,108,111,100,101,77,111,100,105,102,105,101,114,68,97,116,97,0,77,117,108,116,105,114,101,115,77,111,100,105,102,105,101,114,68,97,116,97,0,70,108,117,105,100,115,105,109,77,111,100,105,102,105,101,114,68,97,
116,97,0,70,108,117,105,100,115,105,109,83,101,116,116,105,110,103,115,0,83,109,111,107,101,77,111,100,105,102,105,101,114,68,97,116,97,0,83,104,114,105,110,107,119,114,97,112,77,111,100,105,102,105,101,114,68,97,116,97,0,83,105,109,112,108,101,68,101,102,111,114,109,77,111,100,105,102,105,101,114,68,97,116,97,0,83,104,97,112,101,75,101,121,77,111,100,105,102,105,101,114,68,97,116,97,0,83,111,108,105,100,105,102,121,77,111,100,105,102,
105,101,114,68,97,116,97,0,83,99,114,101,119,77,111,100,105,102,105,101,114,68,97,116,97,0,79,99,101,97,110,77,111,100,105,102,105,101,114,68,97,116,97,0,79,99,101,97,110,0,79,99,101,97,110,67,97,99,104,101,0,87,97,114,112,77,111,100,105,102,105,101,114,68,97,116,97,0,87,101,105,103,104,116,86,71,69,100,105,116,77,111,100,105,102,105,101,114,68,97,116,97,0,87,101,105,103,104,116,86,71,77,105,120,77,111,100,105,102,105,
101,114,68,97,116,97,0,87,101,105,103,104,116,86,71,80,114,111,120,105,109,105,116,121,77,111,100,105,102,105,101,114,68,97,116,97,0,82,101,109,101,115,104,77,111,100,105,102,105,101,114,68,97,116,97,0,83,107,105,110,77,111,100,105,102,105,101,114,68,97,116,97,0,84,114,105,97,110,103,117,108,97,116,101,77,111,100,105,102,105,101,114,68,97,116,97,0,76,97,112,108,97,99,105,97,110,83,109,111,111,116,104,77,111,100,105,102,105,101,114,
68,97,116,97,0,67,111,114,114,101,99,116,105,118,101,83,109,111,111,116,104,68,101,108,116,97,67,97,99,104,101,0,67,111,114,114,101,99,116,105,118,101,83,109,111,111,116,104,77,111,100,105,102,105,101,114,68,97,116,97,0,85,86,87,97,114,112,77,111,100,105,102,105,101,114,68,97,116,97,0,77,101,115,104,67,97,99,104,101,77,111,100,105,102,105,101,114,68,97,116,97,0,76,97,112,108,97,99,105,97,110,68,101,102,111,114,109,77,111,100,
105,102,105,101,114,68,97,116,97,0,87,105,114,101,102,114,97,109,101,77,111,100,105,102,105,101,114,68,97,116,97,0,87,101,108,100,77,111,100,105,102,105,101,114,68,97,116,97,0,68,97,116,97,84,114,97,110,115,102,101,114,77,111,100,105,102,105,101,114,68,97,116,97,0,78,111,114,109,97,108,69,100,105,116,77,111,100,105,102,105,101,114,68,97,116,97,0,77,101,115,104,83,101,113,67,97,99,104,101,77,111,100,105,102,105,101,114,68,97,116,
97,0,83,68,101,102,66,105,110,100,0,83,68,101,102,86,101,114,116,0,83,117,114,102,97,99,101,68,101,102,111,114,109,77,111,100,105,102,105,101,114,68,97,116,97,0,87,101,105,103,104,116,101,100,78,111,114,109,97,108,77,111,100,105,102,105,101,114,68,97,116,97,0,78,111,100,101,115,77,111,100,105,102,105,101,114,83,101,116,116,105,110,103,115,0,78,111,100,101,115,77,111,100,105,102,105,101,114,68,97,116,97,66,108,111,99,107,0,78,111,
100,101,115,77,111,100,105,102,105,101,114,66,97,107,101,70,105,108,101,0,78,111,100,101,115,77,111,100,105,102,105,101,114,80,97,99,107,101,100,66,97,107,101,0,78,111,100,101,115,77,111,100,105,102,105,101,114,66,97,107,101,0,78,111,100,101,115,77,111,100,105,102,105,101,114,80,97,110,101,108,0,78,111,100,101,115,77,111,100,105,102,105,101,114,68,97,116,97,0,78,111,100,101,115,77,111,100,105,102,105,101,114,82,117,110,116,105,109,101,72,
97,110,100,108,101,0,77,101,115,104,84,111,86,111,108,117,109,101,77,111,100,105,102,105,101,114,68,97,116,97,0,86,111,108,117,109,101,68,105,115,112,108,97,99,101,77,111,100,105,102,105,101,114,68,97,116,97,0,86,111,108,117,109,101,84,111,77,101,115,104,77,111,100,105,102,105,101,114,68,97,116,97,0,71,114,101,97,115,101,80,101,110,99,105,108,77,111,100,105,102,105,101,114,73,110,102,108,117,101,110,99,101,68,97,116,97,0,71,114,101,
97,115,101,80,101,110,99,105,108,79,112,97,99,105,116,121,77,111,100,105,102,105,101,114,68,97,116,97,0,71,114,101,97,115,101,80,101,110,99,105,108,83,117,98,100,105,118,77,111,100,105,102,105,101,114,68,97,116,97,0,71,114,101,97,115,101,80,101,110,99,105,108,67,111,108,111,114,77,111,100,105,102,105,101,114,68,97,116,97,0,71,114,101,97,115,101,80,101,110,99,105,108,84,105,110,116,77,111,100,105,102,105,101,114,68,97,116,97,0,71,
114,101,97,115,101,80,101,110,99,105,108,83,109,111,111,116,104,77,111,100,105,102,105,101,114,68,97,116,97,0,71,114,101,97,115,101,80,101,110,99,105,108,79,102,102,115,101,116,77,111,100,105,102,105,101,114,68,97,116,97,0,71,114,101,97,115,101,80,101,110,99,105,108,78,111,105,115,101,77,111,100,105,102,105,101,114,68,97,116,97,0,71,114,101,97,115,101,80,101,110,99,105,108,77,105,114,114,111,114,77,111,100,105,102,105,101,114,68,97,116,
97,0,71,114,101,97,115,101,80,101,110,99,105,108,84,104,105,99,107,77,111,100,105,102,105,101,114,68,97,116,97,0,71,114,101,97,115,101,80,101,110,99,105,108,76,97,116,116,105,99,101,77,111,100,105,102,105,101,114,68,97,116,97,0,71,114,101,97,115,101,80,101,110,99,105,108,68,97,115,104,77,111,100,105,102,105,101,114,83,101,103,109,101,110,116,0,71,114,101,97,115,101,80,101,110,99,105,108,68,97,115,104,77,111,100,105,102,105,101,114,
68,97,116,97,0,71,114,101,97,115,101,80,101,110,99,105,108,77,117,108,116,105,77,111,100,105,102,105,101,114,68,97,116,97,0,71,114,101,97,115,101,80,101,110,99,105,108,76,101,110,103,116,104,77,111,100,105,102,105,101,114,68,97,116,97,0,71,114,101,97,115,101,80,101,110,99,105,108,87,101,105,103,104,116,65,110,103,108,101,77,111,100,105,102,105,101,114,68,97,116,97,0,71,114,101,97,115,101,80,101,110,99,105,108,65,114,114,97,121,77,
111,100,105,102,105,101,114,68,97,116,97,0,71,114,101,97,115,101,80,101,110,99,105,108,87,101,105,103,104,116,80,114,111,120,105,109,105,116,121,77,111,100,105,102,105,101,114,68,97,116,97,0,71,114,101,97,115,101,80,101,110,99,105,108,72,111,111,107,77,111,100,105,102,105,101,114,68,97,116,97,0,71,114,101,97,115,101,80,101,110,99,105,108,76,105,110,101,97,114,116,77,111,100,105,102,105,101,114,68,97,116,97,0,76,105,110,101,97,114,116,
77,111,100,105,102,105,101,114,82,117,110,116,105,109,101,0,71,114,101,97,115,101,80,101,110,99,105,108,65,114,109,97,116,117,114,101,77,111,100,105,102,105,101,114,68,97,116,97,0,71,114,101,97,115,101,80,101,110,99,105,108,84,105,109,101,77,111,100,105,102,105,101,114,83,101,103,109,101,110,116,0,71,114,101,97,115,101,80,101,110,99,105,108,84,105,109,101,77,111,100,105,102,105,101,114,68,97,116,97,0,71,114,101,97,115,101,80,101,110,99,
105,108,69,110,118,101,108,111,112,101,77,111,100,105,102,105,101,114,68,97,116,97,0,71,114,101,97,115,101,80,101,110,99,105,108,79,117,116,108,105,110,101,77,111,100,105,102,105,101,114,68,97,116,97,0,71,114,101,97,115,101,80,101,110,99,105,108,83,104,114,105,110,107,119,114,97,112,77,111,100,105,102,105,101,114,68,97,116,97,0,71,114,101,97,115,101,80,101,110,99,105,108,66,117,105,108,100,77,111,100,105,102,105,101,114,68,97,116,97,0,
71,114,101,97,115,101,80,101,110,99,105,108,83,105,109,112,108,105,102,121,77,111,100,105,102,105,101,114,68,97,116,97,0,71,114,101,97,115,101,80,101,110,99,105,108,84,101,120,116,117,114,101,77,111,100,105,102,105,101,114,68,97,116,97,0,77,111,118,105,101,67,108,105,112,80,114,111,120,121,0,77,111,118,105,101,67,108,105,112,95,82,117,110,116,105,109,101,71,80,85,84,101,120,116,117,114,101,0,77,111,118,105,101,67,108,105,112,95,82,117,
110,116,105,109,101,0,77,111,118,105,101,67,108,105,112,67,97,99,104,101,0,77,111,118,105,101,84,114,97,99,107,105,110,103,0,77,111,118,105,101,67,108,105,112,83,99,111,112,101,115,0,77,111,118,105,101,84,114,97,99,107,105,110,103,77,97,114,107,101,114,0,77,111,118,105,101,84,114,97,99,107,105,110,103,84,114,97,99,107,0,98,65,99,116,105,111,110,77,111,100,105,102,105,101,114,0,98,65,99,116,105,111,110,83,116,114,105,112,0,98,
78,111,100,101,84,114,101,101,73,110,116,101,114,102,97,99,101,73,116,101,109,0,98,78,111,100,101,84,114,101,101,73,110,116,101,114,102,97,99,101,83,111,99,107,101,116,0,98,78,111,100,101,84,114,101,101,73,110,116,101,114,102,97,99,101,80,97,110,101,108,0,98,78,111,100,101,84,114,101,101,73,110,116,101,114,102,97,99,101,0,98,78,111,100,101,84,114,101,101,73,110,116,101,114,102,97,99,101,82,117,110,116,105,109,101,72,97,110,100,108,
101,0,98,78,111,100,101,83,116,97,99,107,0,98,78,111,100,101,83,111,99,107,101,116,0,98,78,111,100,101,83,111,99,107,101,116,84,121,112,101,72,97,110,100,108,101,0,98,78,111,100,101,76,105,110,107,0,98,78,111,100,101,83,111,99,107,101,116,82,117,110,116,105,109,101,72,97,110,100,108,101,0,98,78,111,100,101,80,97,110,101,108,83,116,97,116,101,0,98,78,111,100,101,0,98,78,111,100,101,84,121,112,101,72,97,110,100,108,101,0,
98,78,111,100,101,82,117,110,116,105,109,101,72,97,110,100,108,101,0,98,78,111,100,101,73,110,115,116,97,110,99,101,75,101,121,0,98,78,101,115,116,101,100,78,111,100,101,80,97,116,104,0,98,78,101,115,116,101,100,78,111,100,101,82,101,102,0,98,78,111,100,101,84,114,101,101,84,121,112,101,72,97,110,100,108,101,0,71,101,111,109,101,116,114,121,78,111,100,101,65,115,115,101,116,84,114,97,105,116,115,0,98,78,111,100,101,84,114,101,101,
82,117,110,116,105,109,101,72,97,110,100,108,101,0,98,78,111,100,101,83,111,99,107,101,116,86,97,108,117,101,73,110,116,0,98,78,111,100,101,83,111,99,107,101,116,86,97,108,117,101,70,108,111,97,116,0,98,78,111,100,101,83,111,99,107,101,116,86,97,108,117,101,66,111,111,108,101,97,110,0,98,78,111,100,101,83,111,99,107,101,116,86,97,108,117,101,86,101,99,116,111,114,0,98,78,111,100,101,83,111,99,107,101,116,86,97,108,117,101,82,
111,116,97,116,105,111,110,0,98,78,111,100,101,83,111,99,107,101,116,86,97,108,117,101,82,71,66,65,0,98,78,111,100,101,83,111,99,107,101,116,86,97,108,117,101,83,116,114,105,110,103,0,98,78,111,100,101,83,111,99,107,101,116,86,97,108,117,101,79,98,106,101,99,116,0,98,78,111,100,101,83,111,99,107,101,116,86,97,108,117,101,73,109,97,103,101,0,98,78,111,100,101,83,111,99,107,101,116,86,97,108,117,101,67,111,108,108,101,99,116,
105,111,110,0,98,78,111,100,101,83,111,99,107,101,116,86,97,108,117,101,84,101,120,116,117,114,101,0,98,78,111,100,101,83,111,99,107,101,116,86,97,108,117,101,77,97,116,101,114,105,97,108,0,98,78,111,100,101,83,111,99,107,101,116,86,97,108,117,101,77,101,110,117,0,82,117,110,116,105,109,101,78,111,100,101,69,110,117,109,73,116,101,109,115,72,97,110,100,108,101,0,78,111,100,101,70,114,97,109,101,0,78,111,100,101,82,101,114,111,117,
116,101,0,78,111,100,101,73,109,97,103,101,65,110,105,109,0,67,111,108,111,114,67,111,114,114,101,99,116,105,111,110,68,97,116,97,0,78,111,100,101,67,111,108,111,114,67,111,114,114,101,99,116,105,111,110,0,78,111,100,101,66,111,107,101,104,73,109,97,103,101,0,78,111,100,101,66,111,120,77,97,115,107,0,78,111,100,101,69,108,108,105,112,115,101,77,97,115,107,0,78,111,100,101,73,109,97,103,101,76,97,121,101,114,0,78,111,100,101,66,
108,117,114,68,97,116,97,0,78,111,100,101,68,66,108,117,114,68,97,116,97,0,78,111,100,101,66,105,108,97,116,101,114,97,108,66,108,117,114,68,97,116,97,0,78,111,100,101,75,117,119,97,104,97,114,97,68,97,116,97,0,78,111,100,101,65,110,116,105,65,108,105,97,115,105,110,103,68,97,116,97,0,78,111,100,101,72,117,101,83,97,116,0,78,111,100,101,73,109,97,103,101,70,105,108,101,0,73,109,97,103,101,70,111,114,109,97,116,68,97,
116,97,0,78,111,100,101,73,109,97,103,101,77,117,108,116,105,70,105,108,101,0,78,111,100,101,73,109,97,103,101,77,117,108,116,105,70,105,108,101,83,111,99,107,101,116,0,78,111,100,101,67,104,114,111,109,97,0,78,111,100,101,84,119,111,88,89,115,0,78,111,100,101,84,119,111,70,108,111,97,116,115,0,78,111,100,101,86,101,114,116,101,120,67,111,108,0,78,111,100,101,67,77,80,67,111,109,98,83,101,112,67,111,108,111,114,0,78,111,100,
101,68,101,102,111,99,117,115,0,78,111,100,101,83,99,114,105,112,116,68,105,99,116,0,78,111,100,101,71,108,97,114,101,0,78,111,100,101,84,111,110,101,109,97,112,0,78,111,100,101,76,101,110,115,68,105,115,116,0,78,111,100,101,67,111,108,111,114,66,97,108,97,110,99,101,0,78,111,100,101,67,111,108,111,114,115,112,105,108,108,0,78,111,100,101,67,111,110,118,101,114,116,67,111,108,111,114,83,112,97,99,101,0,78,111,100,101,68,105,108,
97,116,101,69,114,111,100,101,0,78,111,100,101,77,97,115,107,0,78,111,100,101,83,101,116,65,108,112,104,97,0,78,111,100,101,84,101,120,66,97,115,101,0,84,101,120,77,97,112,112,105,110,103,0,67,111,108,111,114,77,97,112,112,105,110,103,0,78,111,100,101,84,101,120,83,107,121,0,78,111,100,101,84,101,120,73,109,97,103,101,0,78,111,100,101,84,101,120,67,104,101,99,107,101,114,0,78,111,100,101,84,101,120,66,114,105,99,107,0,78,
111,100,101,84,101,120,69,110,118,105,114,111,110,109,101,110,116,0,78,111,100,101,84,101,120,71,97,98,111,114,0,78,111,100,101,84,101,120,71,114,97,100,105,101,110,116,0,78,111,100,101,84,101,120,78,111,105,115,101,0,78,111,100,101,84,101,120,86,111,114,111,110,111,105,0,78,111,100,101,84,101,120,77,117,115,103,114,97,118,101,0,78,111,100,101,84,101,120,87,97,118,101,0,78,111,100,101,84,101,120,77,97,103,105,99,0,78,111,100,101,
83,104,97,100,101,114,65,116,116,114,105,98,117,116,101,0,78,111,100,101,83,104,97,100,101,114,86,101,99,116,84,114,97,110,115,102,111,114,109,0,78,111,100,101,83,104,97,100,101,114,84,101,120,80,111,105,110,116,68,101,110,115,105,116,121,0,80,111,105,110,116,68,101,110,115,105,116,121,0,78,111,100,101,83,104,97,100,101,114,80,114,105,110,99,105,112,108,101,100,0,78,111,100,101,83,104,97,100,101,114,72,97,105,114,80,114,105,110,99,105,
112,108,101,100,0,84,101,120,78,111,100,101,79,117,116,112,117,116,0,78,111,100,101,75,101,121,105,110,103,83,99,114,101,101,110,68,97,116,97,0,78,111,100,101,75,101,121,105,110,103,68,97,116,97,0,78,111,100,101,84,114,97,99,107,80,111,115,68,97,116,97,0,78,111,100,101,84,114,97,110,115,108,97,116,101,68,97,116,97,0,78,111,100,101,83,99,97,108,101,68,97,116,97,0,78,111,100,101,80,108,97,110,101,84,114,97,99,107,68,101,
102,111,114,109,68,97,116,97,0,78,111,100,101,83,104,97,100,101,114,83,99,114,105,112,116,0,78,111,100,101,83,104,97,100,101,114,84,97,110,103,101,110,116,0,78,111,100,101,83,104,97,100,101,114,78,111,114,109,97,108,77,97,112,0,78,111,100,101,83,104,97,100,101,114,85,86,77,97,112,0,78,111,100,101,83,104,97,100,101,114,86,101,114,116,101,120,67,111,108,111,114,0,78,111,100,101,83,104,97,100,101,114,84,101,120,73,69,83,0,78,
111,100,101,83,104,97,100,101,114,79,117,116,112,117,116,65,79,86,0,78,111,100,101,83,117,110,66,101,97,109,115,0,67,114,121,112,116,111,109,97,116,116,101,69,110,116,114,121,0,67,114,121,112,116,111,109,97,116,116,101,76,97,121,101,114,0,78,111,100,101,67,114,121,112,116,111,109,97,116,116,101,95,82,117,110,116,105,109,101,0,78,111,100,101,67,114,121,112,116,111,109,97,116,116,101,0,78,111,100,101,68,101,110,111,105,115,101,0,78,111,
100,101,77,97,112,82,97,110,103,101,0,78,111,100,101,82,97,110,100,111,109,86,97,108,117,101,0,78,111,100,101,65,99,99,117,109,117,108,97,116,101,70,105,101,108,100,0,78,111,100,101,73,110,112,117,116,66,111,111,108,0,78,111,100,101,73,110,112,117,116,73,110,116,0,78,111,100,101,73,110,112,117,116,82,111,116,97,116,105,111,110,0,78,111,100,101,73,110,112,117,116,86,101,99,116,111,114,0,78,111,100,101,73,110,112,117,116,67,111,108,
111,114,0,78,111,100,101,73,110,112,117,116,83,116,114,105,110,103,0,78,111,100,101,71,101,111,109,101,116,114,121,69,120,116,114,117,100,101,77,101,115,104,0,78,111,100,101,71,101,111,109,101,116,114,121,79,98,106,101,99,116,73,110,102,111,0,78,111,100,101,71,101,111,109,101,116,114,121,80,111,105,110,116,115,84,111,86,111,108,117,109,101,0,78,111,100,101,71,101,111,109,101,116,114,121,67,111,108,108,101,99,116,105,111,110,73,110,102,111,0,
78,111,100,101,71,101,111,109,101,116,114,121,80,114,111,120,105,109,105,116,121,0,78,111,100,101,71,101,111,109,101,116,114,121,86,111,108,117,109,101,84,111,77,101,115,104,0,78,111,100,101,71,101,111,109,101,116,114,121,77,101,115,104,84,111,86,111,108,117,109,101,0,78,111,100,101,71,101,111,109,101,116,114,121,83,117,98,100,105,118,105,115,105,111,110,83,117,114,102,97,99,101,0,78,111,100,101,71,101,111,109,101,116,114,121,77,101,115,104,67,
105,114,99,108,101,0,78,111,100,101,71,101,111,109,101,116,114,121,77,101,115,104,67,121,108,105,110,100,101,114,0,78,111,100,101,71,101,111,109,101,116,114,121,77,101,115,104,67,111,110,101,0,78,111,100,101,71,101,111,109,101,116,114,121,77,101,114,103,101,66,121,68,105,115,116,97,110,99,101,0,78,111,100,101,71,101,111,109,101,116,114,121,77,101,115,104,76,105,110,101,0,78,111,100,101,83,119,105,116,99,104,0,78,111,100,101,69,110,117,109,
73,116,101,109,0,78,111,100,101,69,110,117,109,68,101,102,105,110,105,116,105,111,110,0,78,111,100,101,77,101,110,117,83,119,105,116,99,104,0,78,111,100,101,71,101,111,109,101,116,114,121,67,117,114,118,101,83,112,108,105,110,101,84,121,112,101,0,78,111,100,101,71,101,111,109,101,116,114,121,83,101,116,67,117,114,118,101,72,97,110,100,108,101,80,111,115,105,116,105,111,110,115,0,78,111,100,101,71,101,111,109,101,116,114,121,67,117,114,118,101,
83,101,116,72,97,110,100,108,101,115,0,78,111,100,101,71,101,111,109,101,116,114,121,67,117,114,118,101,83,101,108,101,99,116,72,97,110,100,108,101,115,0,78,111,100,101,71,101,111,109,101,116,114,121,67,117,114,118,101,80,114,105,109,105,116,105,118,101,65,114,99,0,78,111,100,101,71,101,111,109,101,116,114,121,67,117,114,118,101,80,114,105,109,105,116,105,118,101,76,105,110,101,0,78,111,100,101,71,101,111,109,101,116,114,121,67,117,114,118,101,
80,114,105,109,105,116,105,118,101,66,101,122,105,101,114,83,101,103,109,101,110,116,0,78,111,100,101,71,101,111,109,101,116,114,121,67,117,114,118,101,80,114,105,109,105,116,105,118,101,67,105,114,99,108,101,0,78,111,100,101,71,101,111,109,101,116,114,121,67,117,114,118,101,80,114,105,109,105,116,105,118,101,81,117,97,100,0,78,111,100,101,71,101,111,109,101,116,114,121,67,117,114,118,101,82,101,115,97,109,112,108,101,0,78,111,100,101,71,101,111,
109,101,116,114,121,67,117,114,118,101,70,105,108,108,101,116,0,78,111,100,101,71,101,111,109,101,116,114,121,67,117,114,118,101,84,114,105,109,0,78,111,100,101,71,101,111,109,101,116,114,121,67,117,114,118,101,84,111,80,111,105,110,116,115,0,78,111,100,101,71,101,111,109,101,116,114,121,67,117,114,118,101,83,97,109,112,108,101,0,78,111,100,101,71,101,111,109,101,116,114,121,84,114,97,110,115,102,101,114,65,116,116,114,105,98,117,116,101,0,78,
111,100,101,71,101,111,109,101,116,114,121,83,97,109,112,108,101,73,110,100,101,120,0,78,111,100,101,71,101,111,109,101,116,114,121,82,97,121,99,97,115,116,0,78,111,100,101,71,101,111,109,101,116,114,121,67,117,114,118,101,70,105,108,108,0,78,111,100,101,71,101,111,109,101,116,114,121,77,101,115,104,84,111,80,111,105,110,116,115,0,78,111,100,101,71,101,111,109,101,116,114,121,65,116,116,114,105,98,117,116,101,67,97,112,116,117,114,101,73,116,
101,109,0,78,111,100,101,71,101,111,109,101,116,114,121,65,116,116,114,105,98,117,116,101,67,97,112,116,117,114,101,0,78,111,100,101,71,101,111,109,101,116,114,121,83,116,111,114,101,78,97,109,101,100,65,116,116,114,105,98,117,116,101,0,78,111,100,101,71,101,111,109,101,116,114,121,73,110,112,117,116,78,97,109,101,100,65,116,116,114,105,98,117,116,101,0,78,111,100,101,71,101,111,109,101,116,114,121,83,116,114,105,110,103,84,111,67,117,114,118,
101,115,0,78,111,100,101,71,101,111,109,101,116,114,121,68,101,108,101,116,101,71,101,111,109,101,116,114,121,0,78,111,100,101,71,101,111,109,101,116,114,121,68,117,112,108,105,99,97,116,101,69,108,101,109,101,110,116,115,0,78,111,100,101,71,101,111,109,101,116,114,121,77,101,114,103,101,76,97,121,101,114,115,0,78,111,100,101,71,101,111,109,101,116,114,121,83,101,112,97,114,97,116,101,71,101,111,109,101,116,114,121,0,78,111,100,101,71,101,111,
109,101,116,114,121,73,109,97,103,101,84,101,120,116,117,114,101,0,78,111,100,101,71,101,111,109,101,116,114,121,86,105,101,119,101,114,0,78,111,100,101,71,101,111,109,101,116,114,121,85,86,85,110,119,114,97,112,0,78,111,100,101,83,105,109,117,108,97,116,105,111,110,73,116,101,109,0,78,111,100,101,71,101,111,109,101,116,114,121,83,105,109,117,108,97,116,105,111,110,73,110,112,117,116,0,78,111,100,101,71,101,111,109,101,116,114,121,83,105,109,
117,108,97,116,105,111,110,79,117,116,112,117,116,0,78,111,100,101,82,101,112,101,97,116,73,116,101,109,0,78,111,100,101,71,101,111,109,101,116,114,121,82,101,112,101,97,116,73,110,112,117,116,0,78,111,100,101,71,101,111,109,101,116,114,121,82,101,112,101,97,116,79,117,116,112,117,116,0,78,111,100,101,71,101,111,109,101,116,114,121,70,111,114,101,97,99,104,71,101,111,109,101,116,114,121,69,108,101,109,101,110,116,73,110,112,117,116,0,78,111,
100,101,70,111,114,101,97,99,104,71,101,111,109,101,116,114,121,69,108,101,109,101,110,116,73,110,112,117,116,73,116,101,109,0,78,111,100,101,70,111,114,101,97,99,104,71,101,111,109,101,116,114,121,69,108,101,109,101,110,116,77,97,105,110,73,116,101,109,0,78,111,100,101,70,111,114,101,97,99,104,71,101,111,109,101,116,114,121,69,108,101,109,101,110,116,71,101,110,101,114,97,116,105,111,110,73,116,101,109,0,78,111,100,101,70,111,114,101,97,99,
104,71,101,111,109,101,116,114,121,69,108,101,109,101,110,116,73,110,112,117,116,73,116,101,109,115,0,78,111,100,101,70,111,114,101,97,99,104,71,101,111,109,101,116,114,121,69,108,101,109,101,110,116,77,97,105,110,73,116,101,109,115,0,78,111,100,101,70,111,114,101,97,99,104,71,101,111,109,101,116,114,121,69,108,101,109,101,110,116,71,101,110,101,114,97,116,105,111,110,73,116,101,109,115,0,78,111,100,101,71,101,111,109,101,116,114,121,70,111,114,
101,97,99,104,71,101,111,109,101,116,114,121,69,108,101,109,101,110,116,79,117,116,112,117,116,0,78,111,100,101,71,101,111,109,101,116,114,121,67,108,111,115,117,114,101,73,110,112,117,116,0,78,111,100,101,71,101,111,109,101,116,114,121,67,108,111,115,117,114,101,73,110,112,117,116,73,116,101,109,0,78,111,100,101,71,101,111,109,101,116,114,121,67,108,111,115,117,114,101,79,117,116,112,117,116,73,116,101,109,0,78,111,100,101,71,101,111,109,101,116,
114,121,67,108,111,115,117,114,101,73,110,112,117,116,73,116,101,109,115,0,78,111,100,101,71,101,111,109,101,116,114,121,67,108,111,115,117,114,101,79,117,116,112,117,116,73,116,101,109,115,0,78,111,100,101,71,101,111,109,101,116,114,121,67,108,111,115,117,114,101,79,117,116,112,117,116,0,78,111,100,101,71,101,111,109,101,116,114,121,69,118,97,108,117,97,116,101,67,108,111,115,117,114,101,73,110,112,117,116,73,116,101,109,0,78,111,100,101,71,101,
111,109,101,116,114,121,69,118,97,108,117,97,116,101,67,108,111,115,117,114,101,79,117,116,112,117,116,73,116,101,109,0,78,111,100,101,71,101,111,109,101,116,114,121,69,118,97,108,117,97,116,101,67,108,111,115,117,114,101,73,110,112,117,116,73,116,101,109,115,0,78,111,100,101,71,101,111,109,101,116,114,121,69,118,97,108,117,97,116,101,67,108,111,115,117,114,101,79,117,116,112,117,116,73,116,101,109,115,0,78,111,100,101,71,101,111,109,101,116,114,
121,69,118,97,108,117,97,116,101,67,108,111,115,117,114,101,0,73,110,100,101,120,83,119,105,116,99,104,73,116,101,109,0,78,111,100,101,73,110,100,101,120,83,119,105,116,99,104,0,78,111,100,101,71,101,111,109,101,116,114,121,68,105,115,116,114,105,98,117,116,101,80,111,105,110,116,115,73,110,86,111,108,117,109,101,0,78,111,100,101,70,117,110,99,116,105,111,110,67,111,109,112,97,114,101,0,78,111,100,101,67,111,109,98,83,101,112,67,111,108,
111,114,0,78,111,100,101,83,104,97,100,101,114,77,105,120,0,78,111,100,101,71,101,111,109,101,116,114,121,76,105,110,101,97,114,71,105,122,109,111,0,78,111,100,101,71,101,111,109,101,116,114,121,68,105,97,108,71,105,122,109,111,0,78,111,100,101,71,101,111,109,101,116,114,121,84,114,97,110,115,102,111,114,109,71,105,122,109,111,0,78,111,100,101,71,101,111,109,101,116,114,121,66,97,107,101,73,116,101,109,0,78,111,100,101,71,101,111,109,101,
116,114,121,66,97,107,101,0,78,111,100,101,71,101,111,109,101,116,114,121,67,111,109,98,105,110,101,66,117,110,100,108,101,73,116,101,109,0,78,111,100,101,71,101,111,109,101,116,114,121,67,111,109,98,105,110,101,66,117,110,100,108,101,0,78,111,100,101,71,101,111,109,101,116,114,121,83,101,112,97,114,97,116,101,66,117,110,100,108,101,73,116,101,109,0,78,111,100,101,71,101,111,109,101,116,114,121,83,101,112,97,114,97,116,101,66,117,110,100,108,
101,0,78,111,100,101,70,117,110,99,116,105,111,110,70,111,114,109,97,116,83,116,114,105,110,103,73,116,101,109,0,78,111,100,101,70,117,110,99,116,105,111,110,70,111,114,109,97,116,83,116,114,105,110,103,0,70,108,117,105,100,86,101,114,116,101,120,86,101,108,111,99,105,116,121,0,80,97,114,116,68,101,102,108,101,99,116,0,83,66,86,101,114,116,101,120,0,83,111,102,116,66,111,100,121,95,83,104,97,114,101,100,0,83,111,102,116,66,111,100,
121,0,66,111,100,121,80,111,105,110,116,0,66,111,100,121,83,112,114,105,110,103,0,83,66,83,99,114,97,116,99,104,0,98,68,101,102,111,114,109,71,114,111,117,112,0,98,70,97,99,101,77,97,112,0,79,98,106,101,99,116,76,105,110,101,65,114,116,0,76,105,103,104,116,76,105,110,107,105,110,103,82,117,110,116,105,109,101,0,76,105,103,104,116,76,105,110,107,105,110,103,0,83,99,117,108,112,116,83,101,115,115,105,111,110,0,82,105,103,105,
100,66,111,100,121,79,98,0,82,105,103,105,100,66,111,100,121,67,111,110,0,79,98,106,101,99,116,82,117,110,116,105,109,101,72,97,110,100,108,101,0,79,98,72,111,111,107,0,84,114,101,101,83,116,111,114,101,69,108,101,109,0,84,114,101,101,83,116,111,114,101,0,72,97,105,114,75,101,121,0,80,97,114,116,105,99,108,101,75,101,121,0,66,111,105,100,80,97,114,116,105,99,108,101,0,80,97,114,116,105,99,108,101,83,112,114,105,110,103,0,
67,104,105,108,100,80,97,114,116,105,99,108,101,0,80,97,114,116,105,99,108,101,84,97,114,103,101,116,0,80,97,114,116,105,99,108,101,68,117,112,108,105,87,101,105,103,104,116,0,80,97,114,116,105,99,108,101,68,97,116,97,0,83,80,72,70,108,117,105,100,83,101,116,116,105,110,103,115,0,80,97,114,116,105,99,108,101,83,101,116,116,105,110,103,115,0,80,84,67,97,99,104,101,69,100,105,116,0,80,97,114,116,105,99,108,101,67,97,99,104,
101,75,101,121,0,75,68,84,114,101,101,95,51,100,0,80,97,114,116,105,99,108,101,68,114,97,119,68,97,116,97,0,80,84,67,97,99,104,101,69,120,116,114,97,0,80,84,67,97,99,104,101,77,101,109,0,80,111,105,110,116,67,108,111,117,100,0,80,111,105,110,116,67,108,111,117,100,82,117,110,116,105,109,101,72,97,110,100,108,101,0,82,105,103,105,100,66,111,100,121,87,111,114,108,100,95,83,104,97,114,101,100,0,82,105,103,105,100,66,111,
100,121,87,111,114,108,100,95,82,117,110,116,105,109,101,0,82,105,103,105,100,66,111,100,121,87,111,114,108,100,0,82,105,103,105,100,66,111,100,121,79,98,95,83,104,97,114,101,100,0,70,70,77,112,101,103,67,111,100,101,99,68,97,116,97,0,65,117,100,105,111,68,97,116,97,0,83,99,101,110,101,82,101,110,100,101,114,76,97,121,101,114,0,83,99,101,110,101,82,101,110,100,101,114,86,105,101,119,0,66,97,107,101,68,97,116,97,0,82,101,
110,100,101,114,68,97,116,97,0,84,105,109,101,77,97,114,107,101,114,0,80,97,105,110,116,95,82,117,110,116,105,109,101,0,78,97,109,101,100,66,114,117,115,104,65,115,115,101,116,82,101,102,101,114,101,110,99,101,0,84,111,111,108,83,121,115,116,101,109,66,114,117,115,104,66,105,110,100,105,110,103,115,0,80,97,105,110,116,0,73,109,97,103,101,80,97,105,110,116,83,101,116,116,105,110,103,115,0,80,97,105,110,116,77,111,100,101,83,101,116,
116,105,110,103,115,0,80,97,114,116,105,99,108,101,66,114,117,115,104,68,97,116,97,0,80,97,114,116,105,99,108,101,69,100,105,116,83,101,116,116,105,110,103,115,0,83,99,117,108,112,116,0,67,117,114,118,101,115,83,99,117,108,112,116,0,85,118,83,99,117,108,112,116,0,71,112,80,97,105,110,116,0,71,112,86,101,114,116,101,120,80,97,105,110,116,0,71,112,83,99,117,108,112,116,80,97,105,110,116,0,71,112,87,101,105,103,104,116,80,97,
105,110,116,0,86,80,97,105,110,116,0,71,80,95,83,99,117,108,112,116,95,71,117,105,100,101,0,71,80,95,83,99,117,108,112,116,95,83,101,116,116,105,110,103,115,0,71,80,95,73,110,116,101,114,112,111,108,97,116,101,95,83,101,116,116,105,110,103,115,0,85,110,105,102,105,101,100,80,97,105,110,116,83,101,116,116,105,110,103,115,0,67,111,108,111,114,83,112,97,99,101,72,97,110,100,108,101,0,67,117,114,118,101,80,97,105,110,116,83,101,
116,116,105,110,103,115,0,77,101,115,104,83,116,97,116,86,105,115,0,83,101,113,117,101,110,99,101,114,84,111,111,108,83,101,116,116,105,110,103,115,0,84,111,111,108,83,101,116,116,105,110,103,115,0,85,110,105,116,83,101,116,116,105,110,103,115,0,80,104,121,115,105,99,115,83,101,116,116,105,110,103,115,0,68,105,115,112,108,97,121,83,97,102,101,65,114,101,97,115,0,83,99,101,110,101,68,105,115,112,108,97,121,0,86,105,101,119,51,68,83,
104,97,100,105,110,103,0,82,97,121,116,114,97,99,101,69,69,86,69,69,0,83,99,101,110,101,69,69,86,69,69,0,83,99,101,110,101,71,112,101,110,99,105,108,0,83,99,101,110,101,72,121,100,114,97,0,84,114,97,110,115,102,111,114,109,79,114,105,101,110,116,97,116,105,111,110,83,108,111,116,0,86,105,101,119,51,68,67,117,114,115,111,114,0,69,100,105,116,105,110,103,0,83,99,101,110,101,82,117,110,116,105,109,101,72,97,110,100,108,101,
0,65,82,101,103,105,111,110,0,119,109,84,105,109,101,114,0,119,109,84,111,111,108,116,105,112,83,116,97,116,101,0,83,99,114,86,101,114,116,0,118,101,99,50,115,0,83,99,114,69,100,103,101,0,83,99,114,65,114,101,97,77,97,112,0,76,97,121,111,117,116,80,97,110,101,108,83,116,97,116,101,0,80,97,110,101,108,0,80,97,110,101,108,84,121,112,101,0,117,105,76,97,121,111,117,116,0,80,97,110,101,108,95,82,117,110,116,105,109,101,
0,80,97,110,101,108,67,97,116,101,103,111,114,121,83,116,97,99,107,0,117,105,76,105,115,116,0,117,105,76,105,115,116,84,121,112,101,0,117,105,76,105,115,116,68,121,110,0,117,105,86,105,101,119,83,116,97,116,101,0,117,105,86,105,101,119,83,116,97,116,101,76,105,110,107,0,84,114,97,110,115,102,111,114,109,79,114,105,101,110,116,97,116,105,111,110,0,117,105,80,114,101,118,105,101,119,0,83,99,114,71,108,111,98,97,108,65,114,101,97,
68,97,116,97,0,83,99,114,65,114,101,97,95,82,117,110,116,105,109,101,0,98,84,111,111,108,82,101,102,0,83,99,114,65,114,101,97,0,114,99,116,105,0,83,112,97,99,101,84,121,112,101,0,65,82,101,103,105,111,110,82,117,110,116,105,109,101,72,97,110,100,108,101,0,65,115,115,101,116,83,104,101,108,102,83,101,116,116,105,110,103,115,0,65,115,115,101,116,83,104,101,108,102,0,65,115,115,101,116,83,104,101,108,102,84,121,112,101,0,82,
101,103,105,111,110,65,115,115,101,116,83,104,101,108,102,0,70,105,108,101,72,97,110,100,108,101,114,0,70,105,108,101,72,97,110,100,108,101,114,84,121,112,101,72,97,110,100,108,101,0,83,116,114,105,112,65,110,105,109,0,83,116,114,105,112,69,108,101,109,0,83,116,114,105,112,67,114,111,112,0,83,116,114,105,112,84,114,97,110,115,102,111,114,109,0,83,116,114,105,112,67,111,108,111,114,66,97,108,97,110,99,101,0,83,116,114,105,112,80,114,
111,120,121,0,83,116,114,105,112,0,83,101,113,82,101,116,105,109,105,110,103,72,97,110,100,108,101,0,83,116,114,105,112,82,117,110,116,105,109,101,0,83,101,113,117,101,110,99,101,0,98,83,111,117,110,100,0,77,101,116,97,83,116,97,99,107,0,83,101,113,84,105,109,101,108,105,110,101,67,104,97,110,110,101,108,0,83,101,113,67,111,110,110,101,99,116,105,111,110,0,69,100,105,116,105,110,103,82,117,110,116,105,109,101,0,83,116,114,105,112,
76,111,111,107,117,112,0,77,101,100,105,97,80,114,101,115,101,110,99,101,0,84,104,117,109,98,110,97,105,108,67,97,99,104,101,0,73,110,116,114,97,70,114,97,109,101,67,97,99,104,101,0,83,111,117,114,99,101,73,109,97,103,101,67,97,99,104,101,0,70,105,110,97,108,73,109,97,103,101,67,97,99,104,101,0,80,114,101,102,101,116,99,104,74,111,98,0,87,105,112,101,86,97,114,115,0,71,108,111,119,86,97,114,115,0,84,114,97,110,115,
102,111,114,109,86,97,114,115,0,83,111,108,105,100,67,111,108,111,114,86,97,114,115,0,83,112,101,101,100,67,111,110,116,114,111,108,86,97,114,115,0,71,97,117,115,115,105,97,110,66,108,117,114,86,97,114,115,0,84,101,120,116,86,97,114,115,0,84,101,120,116,86,97,114,115,82,117,110,116,105,109,101,0,67,111,108,111,114,77,105,120,86,97,114,115,0,83,101,113,117,101,110,99,101,77,111,100,105,102,105,101,114,68,97,116,97,0,67,111,108,
111,114,66,97,108,97,110,99,101,77,111,100,105,102,105,101,114,68,97,116,97,0,67,117,114,118,101,115,77,111,100,105,102,105,101,114,68,97,116,97,0,72,117,101,67,111,114,114,101,99,116,77,111,100,105,102,105,101,114,68,97,116,97,0,66,114,105,103,104,116,67,111,110,116,114,97,115,116,77,111,100,105,102,105,101,114,68,97,116,97,0,83,101,113,117,101,110,99,101,114,77,97,115,107,77,111,100,105,102,105,101,114,68,97,116,97,0,87,104,105,
116,101,66,97,108,97,110,99,101,77,111,100,105,102,105,101,114,68,97,116,97,0,83,101,113,117,101,110,99,101,114,84,111,110,101,109,97,112,77,111,100,105,102,105,101,114,68,97,116,97,0,69,81,67,117,114,118,101,77,97,112,112,105,110,103,68,97,116,97,0,83,111,117,110,100,69,113,117,97,108,105,122,101,114,77,111,100,105,102,105,101,114,68,97,116,97,0,83,104,97,100,101,114,70,120,68,97,116,97,0,83,104,97,100,101,114,70,120,68,97,
116,97,95,82,117,110,116,105,109,101,0,68,82,87,83,104,97,100,105,110,103,71,114,111,117,112,0,66,108,117,114,83,104,97,100,101,114,70,120,68,97,116,97,0,67,111,108,111,114,105,122,101,83,104,97,100,101,114,70,120,68,97,116,97,0,70,108,105,112,83,104,97,100,101,114,70,120,68,97,116,97,0,71,108,111,119,83,104,97,100,101,114,70,120,68,97,116,97,0,80,105,120,101,108,83,104,97,100,101,114,70,120,68,97,116,97,0,82,105,109,
83,104,97,100,101,114,70,120,68,97,116,97,0,83,104,97,100,111,119,83,104,97,100,101,114,70,120,68,97,116,97,0,83,119,105,114,108,83,104,97,100,101,114,70,120,68,97,116,97,0,87,97,118,101,83,104,97,100,101,114,70,120,68,97,116,97,0,83,112,97,99,101,73,110,102,111,0,83,112,97,99,101,66,117,116,115,0,83,112,97,99,101,80,114,111,112,101,114,116,105,101,115,95,82,117,110,116,105,109,101,0,83,112,97,99,101,79,111,112,115,
0,83,112,97,99,101,79,117,116,108,105,110,101,114,95,82,117,110,116,105,109,101,0,83,112,97,99,101,71,114,97,112,104,95,82,117,110,116,105,109,101,0,83,112,97,99,101,73,112,111,0,83,112,97,99,101,78,108,97,0,83,101,113,117,101,110,99,101,114,80,114,101,118,105,101,119,79,118,101,114,108,97,121,0,83,101,113,117,101,110,99,101,114,84,105,109,101,108,105,110,101,79,118,101,114,108,97,121,0,83,101,113,117,101,110,99,101,114,67,97,
99,104,101,79,118,101,114,108,97,121,0,83,112,97,99,101,83,101,113,0,83,112,97,99,101,83,101,113,95,82,117,110,116,105,109,101,0,77,97,115,107,83,112,97,99,101,73,110,102,111,0,70,105,108,101,83,101,108,101,99,116,80,97,114,97,109,115,0,70,105,108,101,65,115,115,101,116,83,101,108,101,99,116,80,97,114,97,109,115,0,70,105,108,101,70,111,108,100,101,114,72,105,115,116,111,114,121,0,70,105,108,101,70,111,108,100,101,114,76,105,
115,116,115,0,83,112,97,99,101,70,105,108,101,0,70,105,108,101,76,105,115,116,0,119,109,79,112,101,114,97,116,111,114,0,70,105,108,101,76,97,121,111,117,116,0,83,112,97,99,101,70,105,108,101,95,82,117,110,116,105,109,101,0,83,112,97,99,101,73,109,97,103,101,79,118,101,114,108,97,121,0,83,112,97,99,101,73,109,97,103,101,0,83,112,97,99,101,84,101,120,116,0,83,112,97,99,101,84,101,120,116,95,82,117,110,116,105,109,101,0,
83,99,114,105,112,116,0,83,112,97,99,101,83,99,114,105,112,116,0,98,78,111,100,101,84,114,101,101,80,97,116,104,0,83,112,97,99,101,78,111,100,101,79,118,101,114,108,97,121,0,83,112,97,99,101,78,111,100,101,0,83,112,97,99,101,78,111,100,101,95,82,117,110,116,105,109,101,0,67,111,110,115,111,108,101,76,105,110,101,0,83,112,97,99,101,67,111,110,115,111,108,101,0,83,112,97,99,101,85,115,101,114,80,114,101,102,0,83,112,97,
99,101,67,108,105,112,0,83,112,97,99,101,84,111,112,66,97,114,0,83,112,97,99,101,83,116,97,116,117,115,66,97,114,0,83,112,114,101,97,100,115,104,101,101,116,67,111,108,117,109,110,73,68,0,83,112,114,101,97,100,115,104,101,101,116,67,111,108,117,109,110,0,83,112,114,101,97,100,115,104,101,101,116,67,111,108,117,109,110,82,117,110,116,105,109,101,0,83,112,114,101,97,100,115,104,101,101,116,73,110,115,116,97,110,99,101,73,68,0,83,
112,114,101,97,100,115,104,101,101,116,84,97,98,108,101,73,68,0,83,112,114,101,97,100,115,104,101,101,116,84,97,98,108,101,73,68,71,101,111,109,101,116,114,121,0,86,105,101,119,101,114,80,97,116,104,0,83,112,114,101,97,100,115,104,101,101,116,84,97,98,108,101,0,83,112,97,99,101,83,112,114,101,97,100,115,104,101,101,116,0,83,112,97,99,101,83,112,114,101,97,100,115,104,101,101,116,95,82,117,110,116,105,109,101,0,83,112,114,101,97,
100,115,104,101,101,116,82,111,119,70,105,108,116,101,114,0,83,112,101,97,107,101,114,0,84,101,120,116,76,105,110,101,0,84,101,120,95,82,117,110,116,105,109,101,0,117,105,70,111,110,116,83,116,121,108,101,0,117,105,83,116,121,108,101,0,117,105,87,105,100,103,101,116,67,111,108,111,114,115,0,117,105,87,105,100,103,101,116,83,116,97,116,101,67,111,108,111,114,115,0,117,105,80,97,110,101,108,67,111,108,111,114,115,0,84,104,101,109,101,85,
73,0,84,104,101,109,101,65,115,115,101,116,83,104,101,108,102,0,84,104,101,109,101,83,112,97,99,101,0,84,104,101,109,101,67,111,108,108,101,99,116,105,111,110,67,111,108,111,114,0,84,104,101,109,101,83,116,114,105,112,67,111,108,111,114,0,98,84,104,101,109,101,0,77,111,118,105,101,82,101,99,111,110,115,116,114,117,99,116,101,100,67,97,109,101,114,97,0,77,111,118,105,101,84,114,97,99,107,105,110,103,67,97,109,101,114,97,0,77,111,
118,105,101,84,114,97,99,107,105,110,103,80,108,97,110,101,77,97,114,107,101,114,0,77,111,118,105,101,84,114,97,99,107,105,110,103,80,108,97,110,101,84,114,97,99,107,0,77,111,118,105,101,84,114,97,99,107,105,110,103,83,101,116,116,105,110,103,115,0,77,111,118,105,101,84,114,97,99,107,105,110,103,83,116,97,98,105,108,105,122,97,116,105,111,110,0,77,111,118,105,101,84,114,97,99,107,105,110,103,82,101,99,111,110,115,116,114,117,99,116,
105,111,110,0,77,111,118,105,101,84,114,97,99,107,105,110,103,79,98,106,101,99,116,0,77,111,118,105,101,84,114,97,99,107,105,110,103,83,116,97,116,115,0,77,111,118,105,101,84,114,97,99,107,105,110,103,68,111,112,101,115,104,101,101,116,67,104,97,110,110,101,108,0,77,111,118,105,101,84,114,97,99,107,105,110,103,68,111,112,101,115,104,101,101,116,67,111,118,101,114,97,103,101,83,101,103,109,101,110,116,0,77,111,118,105,101,84,114,97,99,
107,105,110,103,68,111,112,101,115,104,101,101,116,0,98,65,100,100,111,110,0,98,80,97,116,104,67,111,109,112,97,114,101,0,98,85,115,101,114,77,101,110,117,0,98,85,115,101,114,77,101,110,117,73,116,101,109,0,98,85,115,101,114,77,101,110,117,73,116,101,109,95,79,112,0,98,85,115,101,114,77,101,110,117,73,116,101,109,95,77,101,110,117,0,98,85,115,101,114,77,101,110,117,73,116,101,109,95,80,114,111,112,0,98,85,115,101,114,65,115,
115,101,116,76,105,98,114,97,114,121,0,98,85,115,101,114,69,120,116,101,110,115,105,111,110,82,101,112,111,0,83,111,108,105,100,76,105,103,104,116,0,87,97,108,107,78,97,118,105,103,97,116,105,111,110,0,85,115,101,114,68,101,102,95,82,117,110,116,105,109,101,0,85,115,101,114,68,101,102,95,83,112,97,99,101,68,97,116,97,0,85,115,101,114,68,101,102,95,70,105,108,101,83,112,97,99,101,68,97,116,97,0,85,115,101,114,68,101,102,95,
69,120,112,101,114,105,109,101,110,116,97,108,0,98,85,115,101,114,83,99,114,105,112,116,68,105,114,101,99,116,111,114,121,0,98,85,115,101,114,65,115,115,101,116,83,104,101,108,102,83,101,116,116,105,110,103,115,0,85,115,101,114,68,101,102,0,118,101,99,50,102,0,118,101,99,50,105,0,118,101,99,51,105,0,118,101,99,51,102,0,118,101,99,52,102,0,109,97,116,52,120,52,102,0,86,70,111,110,116,68,97,116,97,0,83,109,111,111,116,104,
86,105,101,119,50,68,83,116,111,114,101,0,82,101,103,105,111,110,86,105,101,119,51,68,0,86,105,101,119,82,101,110,100,101,114,0,83,109,111,111,116,104,86,105,101,119,51,68,83,116,111,114,101,0,86,105,101,119,51,68,79,118,101,114,108,97,121,0,86,105,101,119,51,68,95,82,117,110,116,105,109,101,0,86,105,101,119,51,68,0,86,105,101,119,101,114,80,97,116,104,69,108,101,109,0,73,68,86,105,101,119,101,114,80,97,116,104,69,108,101,
109,0,77,111,100,105,102,105,101,114,86,105,101,119,101,114,80,97,116,104,69,108,101,109,0,71,114,111,117,112,78,111,100,101,86,105,101,119,101,114,80,97,116,104,69,108,101,109,0,83,105,109,117,108,97,116,105,111,110,90,111,110,101,86,105,101,119,101,114,80,97,116,104,69,108,101,109,0,82,101,112,101,97,116,90,111,110,101,86,105,101,119,101,114,80,97,116,104,69,108,101,109,0,70,111,114,101,97,99,104,71,101,111,109,101,116,114,121,69,108,
101,109,101,110,116,90,111,110,101,86,105,101,119,101,114,80,97,116,104,69,108,101,109,0,86,105,101,119,101,114,78,111,100,101,86,105,101,119,101,114,80,97,116,104,69,108,101,109,0,69,118,97,108,117,97,116,101,67,108,111,115,117,114,101,78,111,100,101,86,105,101,119,101,114,80,97,116,104,69,108,101,109,0,86,111,108,117,109,101,68,105,115,112,108,97,121,0,86,111,108,117,109,101,82,101,110,100,101,114,0,86,111,108,117,109,101,0,86,111,108,
117,109,101,82,117,110,116,105,109,101,72,97,110,100,108,101,0,82,101,112,111,114,116,76,105,115,116,0,115,116,100,95,109,117,116,101,120,95,116,121,112,101,0,119,109,88,114,68,97,116,97,0,119,109,88,114,82,117,110,116,105,109,101,68,97,116,97,0,88,114,83,101,115,115,105,111,110,83,101,116,116,105,110,103,115,0,119,109,87,105,110,100,111,119,77,97,110,97,103,101,114,0,119,109,87,105,110,100,111,119,0,119,109,75,101,121,67,111,110,102,
105,103,0,85,110,100,111,83,116,97,99,107,0,119,109,77,115,103,66,117,115,0,87,105,110,100,111,119,77,97,110,97,103,101,114,82,117,110,116,105,109,101,72,97,110,100,108,101,0,87,111,114,107,83,112,97,99,101,73,110,115,116,97,110,99,101,72,111,111,107,0,119,109,69,118,101,110,116,95,67,111,110,115,101,99,117,116,105,118,101,68,97,116,97,0,119,109,69,118,101,110,116,0,87,105,110,100,111,119,82,117,110,116,105,109,101,72,97,110,100,
108,101,0,119,109,75,101,121,77,97,112,73,116,101,109,0,80,111,105,110,116,101,114,82,78,65,0,119,109,75,101,121,77,97,112,68,105,102,102,73,116,101,109,0,119,109,75,101,121,77,97,112,0,98,111,111,108,0,119,109,75,101,121,67,111,110,102,105,103,80,114,101,102,0,119,109,79,112,101,114,97,116,111,114,84,121,112,101,0,98,84,111,111,108,82,101,102,95,82,117,110,116,105,109,101,0,87,111,114,107,83,112,97,99,101,76,97,121,111,117,
116,0,119,109,79,119,110,101,114,73,68,0,87,111,114,107,83,112,97,99,101,0,87,111,114,107,83,112,97,99,101,82,117,110,116,105,109,101,72,97,110,100,108,101,0,87,111,114,107,83,112,97,99,101,68,97,116,97,82,101,108,97,116,105,111,110,0,88,114,67,111,109,112,111,110,101,110,116,80,97,116,104,0,88,114,65,99,116,105,111,110,77,97,112,66,105,110,100,105,110,103,0,88,114,85,115,101,114,80,97,116,104,0,88,114,65,99,116,105,111,
110,77,97,112,73,116,101,109,0,88,114,65,99,116,105,111,110,77,97,112,0,0,0,0,84,76,69,78,1,0,1,0,2,0,2,0,4,0,4,0,4,0,4,0,8,0,8,0,8,0,0,0,1,0,0,0,16,0,32,0,64,0,32,0,80,0,24,0,24,0,32,0,16,0,136,0,64,0,224,0,48,0,48,0,0,0,16,0,32,0,0,0,0,0,240,4,152,0,68,4,24,0,0,0,48,0,0,0,16,0,88,0,0,0,0,0,32,0,32,0,
168,0,8,0,100,0,0,0,8,4,232,1,144,4,0,0,24,0,136,0,0,0,4,0,40,0,136,0,32,0,16,0,112,1,88,0,88,0,16,0,112,0,136,1,8,0,88,1,40,0,152,0,120,0,8,1,24,0,0,0,120,0,128,0,24,0,24,0,16,0,24,0,8,0,24,0,16,0,32,0,20,0,104,0,152,3,48,1,0,0,16,0,72,0,32,1,104,0,112,0,240,4,32,0,248,0,232,0,16,0,16,0,128,0,176,1,0,0,24,0,
24,0,80,0,0,0,16,0,8,0,24,0,24,0,24,0,0,0,16,0,24,0,24,0,0,0,56,0,80,0,64,0,104,0,72,0,64,0,20,0,128,0,104,0,0,1,168,1,152,1,48,0,224,8,216,0,0,0,8,3,240,0,28,0,32,0,248,0,76,0,16,16,24,4,128,9,0,0,0,0,24,0,104,0,40,6,40,0,24,10,8,0,96,0,216,0,72,7,40,1,32,0,16,1,0,0,72,0,72,0,4,0,32,0,32,0,160,0,64,0,
72,1,12,0,80,0,40,20,152,20,168,0,64,0,64,0,24,0,56,0,192,0,168,0,184,0,48,0,24,0,88,0,80,0,80,0,80,0,8,0,80,0,88,0,184,0,80,0,80,0,24,0,104,0,104,0,16,0,144,0,232,0,88,0,28,0,32,0,28,0,88,0,24,0,160,0,16,0,152,0,16,8,0,0,36,0,88,0,8,0,16,0,144,2,0,0,72,1,72,0,0,0,248,4,104,3,40,0,72,2,248,0,0,0,120,0,0,4,0,0,
40,0,32,6,96,0,0,0,112,5,248,1,144,0,96,0,184,2,24,0,32,0,136,1,0,0,64,0,80,4,96,1,104,26,240,8,160,0,0,0,0,0,216,0,232,6,56,0,128,0,56,2,32,0,56,0,64,0,12,0,120,0,104,0,124,0,16,0,168,0,192,1,16,0,8,0,48,0,152,0,8,0,160,2,120,0,40,0,48,2,104,0,104,1,8,1,80,1,88,0,224,0,24,1,80,1,216,0,80,1,120,1,80,1,0,0,240,0,96,0,
208,0,8,1,232,1,16,1,152,1,80,1,192,0,24,1,112,1,96,1,80,1,72,1,176,1,0,0,0,0,48,1,0,0,24,1,8,0,88,2,0,0,16,0,120,2,12,0,24,0,32,0,56,0,88,0,248,0,0,0,0,0,40,0,0,0,24,0,0,0,80,4,32,4,88,0,0,0,24,0,136,0,0,0,8,0,0,0,144,0,112,0,184,0,160,1,0,0,48,0,64,0,8,0,88,0,80,0,64,0,0,0,144,1,136,1,40,2,96,1,
160,0,160,0,32,0,128,0,48,0,32,0,32,0,8,0,16,0,168,0,16,0,96,0,104,0,112,0,120,0,112,0,120,0,128,0,120,0,128,0,136,0,120,0,120,0,128,0,120,0,120,0,112,0,112,0,120,0,128,0,104,0,112,0,120,0,112,0,112,0,120,0,104,0,104,0,112,0,112,0,120,0,120,0,104,0,104,0,104,0,104,0,120,0,112,0,128,0,104,0,112,0,16,0,24,0,8,0,32,1,184,0,12,0,16,1,224,0,40,0,
144,0,40,0,152,0,8,0,8,0,12,0,8,0,16,0,12,0,32,0,64,0,4,0,20,0,0,0,4,0,4,0,0,1,1,0,1,0,8,0,16,0,4,0,16,0,24,0,16,0,1,0,1,0,12,0,4,0,104,0,96,0,72,1,120,0,24,1,152,0,208,0,208,0,136,0,208,0,208,0,168,0,128,0,112,1,112,1,40,1,208,0,192,0,216,0,152,1,208,0,120,1,120,0,224,0,0,0,0,0,0,0,208,0,0,0,40,0,0,0,
160,0,152,0,8,0,8,0,112,1,168,0,48,1,216,0,136,0,136,0,216,4,128,0,216,0,216,0,120,0,104,1,168,0,112,5,0,0,0,0,8,2,184,1,240,1,192,1,144,0,128,0,136,0,200,0,32,0,248,0,176,1,224,4,216,0,208,0,192,0,32,1,232,0,144,8,32,0,16,0,56,1,192,0,8,0,32,0,16,0,24,0,72,0,8,0,184,0,0,0,160,0,160,0,224,0,168,0,56,1,56,1,56,1,80,1,56,1,136,1,
80,1,48,1,56,1,48,1,88,0,56,1,72,1,96,1,120,1,120,1,120,1,200,1,208,1,0,0,48,1,80,0,72,1,56,1,64,1,80,1,160,1,56,1,72,1,8,3,48,0,24,0,0,0,128,1,136,0,64,0,208,0,72,0,168,0,8,0,80,0,48,0,64,0,0,0,48,0,16,2,0,0,56,0,0,0,8,0,120,1,0,0,0,0,4,0,8,0,16,0,0,0,4,0,0,0,16,0,16,0,1,0,32,0,12,0,16,0,8,4,
8,0,8,0,8,0,8,0,8,0,16,0,0,0,4,0,64,0,16,0,24,0,104,0,20,0,24,0,24,0,68,0,40,0,28,0,12,0,20,0,12,0,12,0,88,5,80,1,96,5,152,5,44,0,24,0,8,0,64,0,2,0,32,0,16,0,32,0,32,0,12,0,96,0,20,0,128,0,1,0,8,0,1,0,192,3,144,0,48,3,0,4,0,4,192,3,208,3,248,3,200,3,200,3,200,3,216,3,200,3,208,3,200,3,8,1,16,0,208,4,
176,0,4,0,8,0,64,0,68,0,48,0,128,0,4,0,2,0,136,0,80,4,72,0,68,0,64,0,64,0,4,4,64,0,12,0,88,0,80,0,40,0,176,0,4,0,8,0,1,0,2,0,1,0,4,0,12,0,12,0,16,0,8,0,1,0,1,0,2,0,1,0,1,0,1,0,1,0,2,0,1,0,1,0,1,0,1,0,2,0,1,0,24,0,24,0,32,0,1,0,1,0,2,0,2,0,1,0,1,0,1,0,1,0,1,0,2,0,1,0,
1,0,1,0,4,0,4,0,4,0,2,0,1,0,1,0,16,0,24,0,2,0,1,0,4,0,2,0,1,0,1,0,1,0,2,0,2,0,1,0,16,0,4,0,24,0,16,0,4,0,24,0,4,0,16,0,16,0,16,0,24,0,24,0,24,0,80,0,4,0,16,0,16,0,24,0,24,0,48,0,16,0,16,0,24,0,24,0,48,0,4,0,24,0,1,0,4,0,1,0,8,0,8,0,4,0,4,0,24,0,24,0,16,0,24,0,16,0,24,0,
16,0,24,0,12,0,232,0,16,0,24,0,224,1,0,0,0,0,0,0,88,0,88,0,16,0,24,0,40,0,0,0,88,0,152,0,0,0,0,1,16,0,16,0,36,0,56,0,56,0,16,0,64,0,40,0,32,0,200,0,68,0,216,3,0,0,0,0,0,0,0,0,32,0,112,0,40,2,0,0,32,0,0,0,88,0,0,0,80,0,32,0,192,0,152,0,120,5,32,17,104,0,16,0,32,0,24,0,128,0,208,0,56,0,16,0,176,0,224,0,
128,0,24,0,136,0,136,0,136,0,136,0,144,0,40,0,80,0,8,0,208,0,0,0,32,0,40,0,20,0,128,4,16,0,24,0,32,0,216,3,176,3,24,0,224,0,16,0,8,0,16,0,64,0,176,12,0,0,48,1,0,0,0,0,32,0,4,0,40,0,48,0,32,0,200,0,0,0,0,0,0,0,80,0,72,1,0,0,0,0,8,0,88,0,120,0,88,0,12,0,16,0,232,0,184,0,16,0,0,0,0,0,104,0,200,0,0,0,24,0,
8,0,0,0,24,0,12,1,16,0,32,0,84,0,24,4,136,3,40,0,8,0,208,1,80,5,48,0,88,0,24,0,48,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,12,0,24,0,32,0,16,0,32,0,8,0,144,2,0,0,8,0,112,0,200,0,24,2,24,2,120,0,112,0,128,0,144,0,184,1,128,0,104,0,40,0,0,0,168,0,192,0,152,0,208,0,176,0,200,0,224,0,168,0,168,0,48,0,248,0,0,0,56,1,0,0,
24,0,248,0,208,0,8,0,8,0,8,0,24,1,0,0,16,0,40,8,80,8,56,0,0,0,160,0,0,0,168,0,0,0,0,0,8,0,136,41,88,2,0,0,16,6,64,0,168,0,8,0,112,1,0,0,40,0,120,1,112,0,160,1,40,0,40,0,8,0,40,0,0,0,4,0,4,0,48,0,16,0,32,0,144,0,0,0,152,0,32,1,40,0,8,0,32,0,232,0,40,0,64,0,16,0,224,3,8,0,176,3,4,0,4,0,24,76,72,0,
96,0,40,0,128,0,64,0,72,0,24,0,168,0,0,1,120,0,32,0,48,0,152,0,24,3,104,0,88,0,232,0,152,0,160,1,88,4,144,8,56,0,32,0,8,0,8,0,40,0,24,0,80,3,96,0,16,53,8,0,8,0,12,0,12,0,16,0,64,0,0,0,0,0,176,3,0,0,0,0,128,0,32,0,120,5,32,0,40,0,40,0,40,0,40,0,40,0,40,0,40,0,48,0,32,0,16,0,160,5,0,0,48,0,0,0,240,3,0,0,
232,3,160,5,120,1,168,0,0,0,0,0,0,0,32,0,0,0,0,0,0,0,184,0,0,0,32,0,16,1,0,0,88,0,0,0,0,0,88,0,144,0,88,1,0,0,40,0,208,0,128,1,80,0,32,1,104,0,0,0,83,84,82,67,189,3,0,0,13,0,0,0,14,0,3,0,0,0,0,0,4,0,1,0,0,0,2,0,15,0,5,0,0,0,3,0,0,0,4,0,0,0,0,0,4,0,5,0,4,0,6,0,16,0,11,0,14,0,7,0,
4,0,8,0,4,0,9,0,4,0,10,0,4,0,11,0,4,0,12,0,4,0,13,0,4,0,14,0,4,0,15,0,4,0,16,0,15,0,17,0,17,0,5,0,14,0,7,0,12,0,8,0,4,0,9,0,0,0,18,0,12,0,15,0,18,0,11,0,14,0,7,0,8,0,8,0,4,0,9,0,0,0,2,0,7,0,14,0,4,0,19,0,8,0,10,0,8,0,11,0,8,0,12,0,8,0,13,0,8,0,15,0,19,0,2,0,14,0,7,0,
0,0,20,0,20,0,3,0,14,0,7,0,2,0,21,0,0,0,22,0,21,0,4,0,11,0,23,0,22,0,24,0,4,0,25,0,4,0,26,0,23,0,11,0,23,0,27,0,23,0,28,0,0,0,29,0,0,0,30,0,2,0,31,0,0,0,32,0,0,0,33,0,21,0,34,0,4,0,35,0,4,0,36,0,14,0,37,0,24,0,12,0,24,0,27,0,24,0,28,0,2,0,38,0,2,0,31,0,2,0,39,0,0,0,40,0,0,0,41,0,
0,0,42,0,4,0,43,0,4,0,44,0,25,0,45,0,25,0,46,0,26,0,7,0,26,0,27,0,26,0,28,0,0,0,47,0,22,0,48,0,2,0,39,0,0,0,49,0,4,0,50,0,27,0,6,0,25,0,51,0,22,0,52,0,25,0,53,0,28,0,54,0,4,0,31,0,0,0,55,0,29,0,4,0,4,0,56,0,4,0,57,0,4,0,58,0,4,0,59,0,30,0,3,0,29,0,60,0,31,0,61,0,32,0,62,0,25,0,22,0,
11,0,27,0,11,0,28,0,25,0,63,0,33,0,64,0,34,0,65,0,0,0,66,0,2,0,31,0,4,0,39,0,4,0,67,0,4,0,68,0,4,0,69,0,4,0,70,0,4,0,71,0,4,0,72,0,23,0,73,0,23,0,74,0,11,0,75,0,27,0,76,0,25,0,77,0,11,0,78,0,35,0,79,0,30,0,80,0,33,0,4,0,25,0,81,0,0,0,82,0,36,0,83,0,37,0,54,0,35,0,3,0,0,0,84,0,0,0,85,0,
0,0,49,0,38,0,6,0,4,0,86,0,4,0,87,0,2,0,88,0,2,0,89,0,4,0,90,0,39,0,54,0,40,0,2,0,7,0,91,0,4,0,31,0,41,0,13,0,40,0,92,0,4,0,93,0,4,0,94,0,4,0,95,0,7,0,96,0,7,0,97,0,4,0,98,0,4,0,31,0,0,0,99,0,42,0,100,0,43,0,101,0,43,0,102,0,11,0,103,0,44,0,11,0,2,0,69,0,2,0,104,0,2,0,105,0,2,0,106,0,
2,0,107,0,2,0,108,0,0,0,2,0,4,0,109,0,4,0,110,0,4,0,111,0,4,0,112,0,45,0,4,0,7,0,113,0,7,0,114,0,7,0,115,0,7,0,116,0,46,0,11,0,47,0,72,0,48,0,117,0,4,0,118,0,7,0,119,0,0,0,120,0,49,0,121,0,49,0,122,0,49,0,123,0,48,0,124,0,45,0,125,0,11,0,103,0,50,0,71,0,50,0,27,0,50,0,28,0,23,0,126,0,23,0,74,0,22,0,127,0,
0,0,32,0,2,0,31,0,2,0,128,0,2,0,129,0,2,0,130,0,0,0,131,0,0,0,132,0,0,0,133,0,0,0,134,0,0,0,33,0,51,0,135,0,50,0,136,0,50,0,137,0,22,0,138,0,22,0,139,0,41,0,140,0,52,0,141,0,50,0,142,0,7,0,143,0,7,0,144,0,7,0,145,0,7,0,146,0,7,0,147,0,7,0,148,0,7,0,149,0,7,0,150,0,7,0,151,0,7,0,152,0,7,0,153,0,2,0,154,0,
0,0,22,0,7,0,155,0,7,0,156,0,7,0,157,0,7,0,158,0,7,0,159,0,7,0,160,0,7,0,161,0,7,0,162,0,7,0,163,0,7,0,164,0,7,0,165,0,7,0,166,0,7,0,167,0,7,0,168,0,7,0,169,0,7,0,170,0,7,0,171,0,7,0,172,0,7,0,173,0,7,0,174,0,7,0,175,0,7,0,176,0,7,0,177,0,7,0,178,0,7,0,179,0,7,0,180,0,7,0,181,0,50,0,182,0,50,0,183,0,
11,0,184,0,53,0,185,0,50,0,186,0,54,0,187,0,11,0,188,0,46,0,80,0,55,0,14,0,22,0,189,0,56,0,190,0,50,0,191,0,2,0,31,0,0,0,49,0,7,0,192,0,7,0,193,0,7,0,194,0,22,0,195,0,4,0,196,0,4,0,197,0,11,0,198,0,11,0,199,0,44,0,200,0,57,0,1,0,4,0,197,0,58,0,12,0,4,0,197,0,7,0,19,0,2,0,201,0,2,0,202,0,7,0,203,0,7,0,204,0,
2,0,205,0,2,0,31,0,7,0,206,0,7,0,207,0,7,0,208,0,7,0,209,0,59,0,10,0,59,0,27,0,59,0,28,0,22,0,210,0,4,0,211,0,4,0,212,0,60,0,213,0,4,0,31,0,4,0,214,0,0,0,32,0,61,0,215,0,62,0,21,0,25,0,81,0,63,0,216,0,4,0,217,0,4,0,218,0,64,0,219,0,4,0,220,0,4,0,221,0,65,0,222,0,4,0,223,0,0,0,33,0,22,0,224,0,22,0,189,0,
22,0,225,0,22,0,226,0,4,0,31,0,4,0,227,0,4,0,228,0,0,0,120,0,7,0,229,0,7,0,230,0,38,0,231,0,66,0,8,0,25,0,232,0,22,0,189,0,67,0,233,0,0,0,234,0,4,0,235,0,4,0,236,0,4,0,31,0,4,0,237,0,68,0,2,0,0,0,31,0,0,0,238,0,69,0,19,0,70,0,27,0,70,0,28,0,22,0,239,0,0,0,240,0,0,0,241,0,0,0,242,0,71,0,243,0,62,0,244,0,
4,0,245,0,0,0,99,0,66,0,246,0,7,0,247,0,2,0,31,0,0,0,248,0,0,0,249,0,0,0,250,0,0,0,251,0,0,0,252,0,68,0,80,0,72,0,8,0,72,0,27,0,72,0,28,0,59,0,253,0,73,0,254,0,22,0,255,0,4,0,31,0,0,0,32,0,4,0,0,1,63,0,8,0,0,0,32,0,7,0,1,1,1,0,2,1,12,0,3,1,1,0,40,0,74,0,4,1,4,0,5,1,1,0,120,0,64,0,6,0,
0,0,66,0,2,0,6,1,4,0,7,1,12,0,8,1,1,0,9,1,75,0,54,0,74,0,7,0,12,0,10,1,1,0,11,1,4,0,12,1,7,0,229,0,7,0,230,0,7,0,13,1,1,0,120,0,65,0,3,0,60,0,14,1,4,0,15,1,1,0,2,0,60,0,6,0,4,0,16,1,4,0,17,1,59,0,18,1,1,0,2,0,4,0,19,1,76,0,20,1,77,0,14,0,77,0,27,0,77,0,28,0,76,0,21,1,11,0,22,1,
0,0,32,0,2,0,29,0,2,0,31,0,2,0,23,1,0,0,22,0,7,0,1,1,7,0,24,1,7,0,25,1,7,0,26,1,7,0,27,1,78,0,5,0,7,0,28,1,4,0,29,1,4,0,30,1,4,0,248,0,4,0,31,0,79,0,6,0,7,0,31,1,7,0,32,1,7,0,33,1,7,0,34,1,4,0,29,0,4,0,31,0,80,0,5,0,7,0,10,0,7,0,11,0,7,0,35,1,2,0,36,1,2,0,37,1,81,0,5,0,
80,0,22,1,4,0,38,1,7,0,39,1,7,0,10,0,7,0,11,0,82,0,4,0,2,0,40,1,2,0,41,1,2,0,42,1,2,0,43,1,83,0,3,0,84,0,44,1,4,0,31,0,0,0,2,0,85,0,10,0,7,0,45,1,7,0,46,1,7,0,47,1,7,0,48,1,7,0,49,1,7,0,50,1,2,0,51,1,2,0,52,1,0,0,53,1,0,0,18,0,86,0,5,0,7,0,54,1,7,0,48,1,7,0,94,0,7,0,95,0,
4,0,31,0,87,0,11,0,25,0,55,1,0,0,47,0,0,0,56,1,2,0,57,1,0,0,58,1,0,0,59,1,2,0,31,0,2,0,60,1,4,0,6,1,4,0,61,1,7,0,62,1,88,0,8,0,88,0,27,0,88,0,28,0,0,0,32,0,87,0,63,1,0,0,64,1,0,0,29,0,2,0,31,0,7,0,65,1,89,0,8,0,22,0,66,1,0,0,67,1,11,0,68,1,90,0,69,1,7,0,65,1,7,0,1,1,4,0,29,0,
4,0,31,0,91,0,3,0,7,0,70,1,4,0,31,0,0,0,2,0,76,0,20,0,76,0,27,0,76,0,28,0,59,0,253,0,89,0,71,1,22,0,72,1,92,0,73,1,91,0,74,1,4,0,38,1,4,0,75,1,7,0,65,1,2,0,31,0,2,0,76,1,0,0,77,1,0,0,18,0,4,0,78,1,0,0,47,0,4,0,79,1,7,0,96,0,7,0,80,1,7,0,81,1,93,0,29,0,93,0,27,0,93,0,28,0,22,0,82,1,
62,0,83,1,4,0,245,0,0,0,84,1,0,0,40,0,22,0,85,1,22,0,72,1,0,0,32,0,7,0,1,1,7,0,86,1,7,0,87,1,7,0,88,1,7,0,89,1,7,0,90,1,7,0,91,1,7,0,92,1,7,0,26,1,7,0,27,1,2,0,93,1,2,0,94,1,0,0,95,1,2,0,29,0,11,0,96,1,4,0,31,0,0,0,99,0,93,0,97,1,11,0,98,1,94,0,6,0,94,0,27,0,94,0,28,0,22,0,82,1,
4,0,31,0,4,0,99,1,0,0,32,0,95,0,11,0,95,0,27,0,95,0,28,0,25,0,55,1,0,0,100,1,4,0,6,1,2,0,101,1,2,0,31,0,0,0,47,0,4,0,78,1,2,0,102,1,2,0,103,1,96,0,12,0,96,0,27,0,96,0,28,0,22,0,104,1,0,0,105,1,0,0,32,0,0,0,106,1,0,0,107,1,4,0,108,1,2,0,31,0,2,0,102,1,2,0,103,1,0,0,22,0,97,0,5,0,97,0,27,0,
97,0,28,0,0,0,47,0,4,0,78,1,7,0,5,0,98,0,19,0,62,0,244,0,4,0,16,1,0,0,109,1,1,0,40,0,62,0,110,1,4,0,111,1,0,0,112,1,1,0,95,1,22,0,113,1,94,0,114,1,93,0,115,1,22,0,116,1,22,0,117,1,76,0,118,1,4,0,31,0,2,0,119,1,2,0,120,1,7,0,121,1,1,0,99,0,99,0,2,0,25,0,81,0,98,0,122,1,54,0,3,0,12,0,123,1,1,0,238,0,
61,0,124,1,100,0,1,0,22,0,125,1,51,0,56,0,51,0,27,0,51,0,28,0,23,0,126,0,23,0,74,0,11,0,126,1,51,0,136,0,22,0,127,1,0,0,32,0,7,0,128,1,7,0,129,1,7,0,130,1,7,0,131,1,4,0,31,0,12,0,132,1,0,0,133,1,54,0,187,0,0,0,134,1,0,0,18,0,7,0,135,1,7,0,136,1,7,0,137,1,7,0,138,1,7,0,139,1,7,0,140,1,7,0,141,1,7,0,93,0,
7,0,142,1,7,0,143,1,7,0,144,1,7,0,168,0,7,0,169,0,7,0,170,0,7,0,171,0,7,0,172,0,7,0,173,0,7,0,174,0,7,0,175,0,7,0,176,0,7,0,177,0,7,0,178,0,7,0,179,0,7,0,180,0,7,0,181,0,7,0,149,0,4,0,145,1,2,0,146,1,0,0,147,1,0,0,148,1,0,0,149,1,0,0,150,1,4,0,151,1,2,0,152,1,2,0,153,1,51,0,182,0,51,0,183,0,100,0,80,0,
101,0,3,0,4,0,154,1,1,0,33,0,102,0,155,1,103,0,24,0,25,0,81,0,98,0,122,1,22,0,156,1,56,0,157,1,11,0,75,0,22,0,158,1,51,0,159,1,104,0,160,1,0,0,161,1,0,0,11,1,4,0,31,0,4,0,132,1,2,0,162,1,2,0,163,1,22,0,125,1,102,0,164,1,4,0,165,1,4,0,166,1,0,0,167,1,4,0,168,1,4,0,145,1,4,0,169,1,7,0,170,1,101,0,80,0,102,0,10,0,
102,0,27,0,102,0,28,0,0,0,32,0,22,0,171,1,1,0,172,1,1,0,238,0,4,0,173,1,4,0,174,1,23,0,126,0,23,0,74,0,105,0,3,0,105,0,27,0,105,0,28,0,51,0,135,0,106,0,3,0,106,0,27,0,106,0,28,0,102,0,175,1,107,0,3,0,107,0,27,0,107,0,28,0,0,0,32,0,34,0,12,0,108,0,176,1,23,0,73,0,109,0,177,1,0,0,178,1,0,0,179,1,0,0,0,0,0,0,180,1,
0,0,181,1,22,0,182,1,2,0,183,1,2,0,184,1,0,0,2,0,110,0,3,0,2,0,29,0,0,0,95,1,4,0,185,1,111,0,4,0,0,0,22,0,2,0,186,1,0,0,187,1,0,0,188,1,112,0,3,0,112,0,27,0,112,0,28,0,0,0,189,1,113,0,3,0,11,0,22,1,114,0,190,1,9,0,45,1,115,0,2,0,11,0,22,1,114,0,190,1,116,0,6,0,0,0,4,0,2,0,191,1,12,0,192,1,12,0,193,1,
0,0,2,0,11,0,22,1,117,0,4,0,116,0,194,1,4,0,195,1,0,0,2,0,118,0,54,0,119,0,5,0,119,0,27,0,119,0,28,0,4,0,29,0,4,0,31,0,0,0,196,1,120,0,6,0,119,0,197,1,52,0,198,1,4,0,60,1,7,0,199,1,4,0,200,1,4,0,210,0,121,0,3,0,119,0,197,1,4,0,60,1,7,0,201,1,122,0,8,0,119,0,197,1,52,0,198,1,7,0,148,0,7,0,202,1,7,0,203,1,
7,0,204,1,4,0,60,1,4,0,205,1,123,0,5,0,119,0,197,1,7,0,206,1,7,0,207,1,7,0,208,1,0,0,33,0,124,0,3,0,119,0,197,1,7,0,204,1,7,0,209,1,125,0,4,0,7,0,210,1,7,0,211,1,2,0,212,1,2,0,248,0,126,0,14,0,126,0,27,0,126,0,28,0,22,0,213,1,22,0,214,1,22,0,215,1,0,0,196,1,4,0,81,0,4,0,31,0,4,0,216,1,7,0,217,1,4,0,200,1,
4,0,210,0,7,0,218,1,7,0,219,1,127,0,23,0,4,0,60,1,4,0,220,1,7,0,221,1,7,0,222,1,7,0,223,1,7,0,224,1,7,0,210,1,7,0,225,1,7,0,46,1,7,0,226,1,7,0,227,1,7,0,228,1,7,0,229,1,7,0,230,1,7,0,231,1,7,0,232,1,7,0,233,1,7,0,234,1,7,0,235,1,7,0,236,1,7,0,237,1,7,0,238,1,22,0,239,1,128,0,57,0,7,0,240,1,7,0,241,1,
7,0,242,1,7,0,243,1,7,0,244,1,7,0,245,1,7,0,246,1,7,0,247,1,2,0,248,1,2,0,249,1,2,0,250,1,2,0,251,1,7,0,252,1,0,0,253,1,12,0,254,1,0,0,255,1,4,0,0,2,4,0,1,2,4,0,2,2,4,0,3,2,4,0,4,2,7,0,5,2,4,0,6,2,4,0,7,2,7,0,8,2,7,0,9,2,7,0,10,2,4,0,31,0,7,0,11,2,7,0,12,2,7,0,13,2,7,0,14,2,
4,0,15,2,4,0,16,2,4,0,17,2,2,0,18,2,2,0,19,2,7,0,20,2,7,0,21,2,7,0,22,2,4,0,23,2,0,0,120,0,7,0,24,2,4,0,25,2,129,0,26,2,129,0,27,2,129,0,28,2,129,0,29,2,129,0,30,2,129,0,31,2,129,0,32,2,129,0,33,2,129,0,34,2,7,0,35,2,7,0,36,2,130,0,37,2,130,0,38,2,131,0,11,0,4,0,39,2,4,0,40,2,4,0,31,0,7,0,41,2,
7,0,42,2,7,0,43,2,7,0,44,2,4,0,45,2,1,0,46,2,0,0,47,2,129,0,48,2,132,0,134,0,25,0,81,0,129,0,21,1,133,0,49,2,133,0,50,2,132,0,51,2,134,0,52,2,38,0,231,0,135,0,53,2,136,0,54,2,0,0,55,2,7,0,56,2,7,0,57,2,2,0,58,2,2,0,59,2,7,0,140,1,4,0,45,1,4,0,31,0,4,0,0,2,4,0,60,2,4,0,4,2,4,0,61,2,7,0,62,2,
4,0,63,2,4,0,64,2,4,0,65,2,4,0,66,2,7,0,67,2,7,0,68,2,7,0,69,2,4,0,23,2,7,0,70,2,129,0,32,2,129,0,33,2,129,0,34,2,7,0,71,2,7,0,72,2,7,0,73,2,7,0,74,2,7,0,75,2,7,0,76,2,4,0,77,2,7,0,78,2,7,0,79,2,7,0,80,2,7,0,81,2,4,0,82,2,4,0,83,2,7,0,115,0,4,0,84,2,0,0,85,2,0,0,86,2,0,0,87,2,
0,0,88,2,7,0,89,2,0,0,90,2,0,0,91,2,0,0,92,2,0,0,93,2,0,0,94,2,0,0,95,2,0,0,96,2,0,0,97,2,0,0,98,2,0,0,99,2,0,0,100,2,7,0,101,2,7,0,102,2,7,0,103,2,7,0,104,2,7,0,105,2,7,0,106,2,7,0,107,2,7,0,108,2,7,0,222,1,7,0,109,2,7,0,110,2,7,0,111,2,7,0,112,2,4,0,113,2,7,0,114,2,4,0,115,2,7,0,116,2,
4,0,117,2,4,0,118,2,4,0,119,2,7,0,120,2,7,0,121,2,7,0,122,2,7,0,123,2,4,0,124,2,7,0,125,2,4,0,126,2,4,0,127,2,7,0,128,2,4,0,129,2,4,0,130,2,4,0,131,2,4,0,132,2,4,0,133,2,7,0,134,2,4,0,135,2,4,0,136,2,4,0,137,2,7,0,138,2,7,0,139,2,7,0,140,2,7,0,141,2,7,0,142,2,4,0,143,2,7,0,144,2,7,0,145,2,4,0,146,2,
7,0,147,2,4,0,148,2,4,0,149,2,4,0,150,2,4,0,151,2,4,0,152,2,7,0,153,2,7,0,154,2,4,0,155,2,4,0,156,2,7,0,252,1,7,0,157,2,7,0,158,2,7,0,159,2,7,0,160,2,7,0,161,2,7,0,162,2,128,0,163,2,131,0,164,2,4,0,165,2,7,0,166,2,129,0,167,2,137,0,5,0,7,0,69,2,7,0,5,0,7,0,168,2,7,0,169,2,7,0,170,2,138,0,4,0,138,0,27,0,
138,0,28,0,7,0,69,2,7,0,5,0,139,0,4,0,25,0,81,0,22,0,171,2,4,0,172,2,0,0,2,0,140,0,2,0,92,0,173,2,7,0,174,2,136,0,4,0,25,0,81,0,140,0,92,0,4,0,175,2,4,0,176,2,141,0,3,0,141,0,27,0,141,0,28,0,0,0,177,2,142,0,5,0,142,0,27,0,142,0,28,0,0,0,178,2,4,0,31,0,4,0,179,2,143,0,26,0,25,0,81,0,98,0,122,1,22,0,180,2,
22,0,181,2,0,0,178,2,0,0,182,2,0,0,183,2,0,0,184,2,0,0,185,2,7,0,92,1,7,0,186,2,7,0,13,1,0,0,2,0,2,0,31,0,0,0,29,0,0,0,187,2,0,0,133,1,0,0,188,2,4,0,189,2,4,0,190,2,0,0,191,2,0,0,192,2,0,0,193,2,144,0,194,2,0,0,195,2,145,0,196,2,146,0,8,0,7,0,197,2,7,0,198,2,2,0,199,2,2,0,200,2,2,0,31,0,0,0,49,0,
7,0,201,2,7,0,202,2,147,0,12,0,147,0,27,0,147,0,28,0,148,0,203,2,149,0,204,2,150,0,205,2,151,0,206,2,7,0,207,2,7,0,92,1,7,0,208,2,7,0,71,2,2,0,31,0,2,0,209,2,152,0,9,0,52,0,210,2,0,0,211,2,7,0,212,2,7,0,213,2,7,0,214,2,7,0,215,2,4,0,216,2,2,0,31,0,0,0,49,0,153,0,5,0,7,0,217,2,7,0,218,2,7,0,219,2,7,0,220,2,
7,0,221,2,154,0,49,0,25,0,81,0,98,0,122,1,0,0,29,0,0,0,222,2,2,0,31,0,7,0,223,2,7,0,224,2,7,0,225,2,7,0,226,2,7,0,227,2,7,0,228,2,7,0,229,2,7,0,230,2,7,0,231,2,7,0,232,2,7,0,233,2,0,0,234,2,0,0,235,2,0,0,49,0,7,0,236,2,7,0,237,2,7,0,238,2,7,0,239,2,7,0,240,2,7,0,241,2,7,0,242,2,7,0,243,2,7,0,244,2,
7,0,245,2,7,0,246,2,7,0,247,2,7,0,248,2,7,0,249,2,7,0,250,2,7,0,251,2,7,0,252,2,155,0,253,2,0,0,254,2,0,0,255,2,0,0,0,3,4,0,1,3,4,0,2,3,73,0,254,0,52,0,3,3,156,0,4,3,152,0,5,3,22,0,6,3,146,0,7,3,153,0,80,0,157,0,70,0,158,0,8,3,7,0,9,3,7,0,10,3,7,0,11,3,7,0,12,3,7,0,13,3,7,0,14,3,7,0,15,3,
7,0,16,3,7,0,17,3,7,0,18,3,7,0,19,3,7,0,20,3,7,0,21,3,7,0,22,3,7,0,23,3,7,0,24,3,7,0,25,3,7,0,26,3,7,0,27,3,7,0,28,3,7,0,29,3,7,0,30,3,7,0,31,3,7,0,32,3,7,0,33,3,7,0,34,3,7,0,35,3,7,0,36,3,7,0,37,3,7,0,38,3,7,0,39,3,7,0,40,3,7,0,41,3,7,0,42,3,2,0,43,3,0,0,44,3,7,0,45,3,
7,0,46,3,4,0,47,3,4,0,172,1,4,0,48,3,4,0,49,3,2,0,50,3,2,0,51,3,2,0,52,3,2,0,53,3,2,0,54,3,2,0,55,3,2,0,56,3,2,0,57,3,159,0,58,3,2,0,59,3,2,0,60,3,7,0,61,3,7,0,62,3,7,0,63,3,7,0,64,3,7,0,65,3,7,0,66,3,7,0,67,3,7,0,68,3,7,0,69,3,2,0,70,3,0,0,95,1,7,0,71,3,7,0,72,3,7,0,73,3,
7,0,74,3,0,0,33,0,160,0,18,0,158,0,75,3,7,0,76,3,7,0,77,3,7,0,78,3,7,0,79,3,7,0,80,3,7,0,81,3,7,0,82,3,4,0,172,1,2,0,83,3,2,0,84,3,0,0,2,0,67,0,85,3,2,0,86,3,2,0,87,3,0,0,99,0,7,0,88,3,7,0,89,3,161,0,2,0,1,0,90,3,1,0,18,0,162,0,5,0,162,0,27,0,162,0,28,0,52,0,198,1,161,0,91,3,4,0,179,2,
163,0,5,0,163,0,27,0,163,0,28,0,67,0,92,3,161,0,91,3,4,0,179,2,164,0,7,0,164,0,27,0,164,0,28,0,0,0,93,3,0,0,32,0,23,0,94,3,4,0,31,0,4,0,95,3,165,0,6,0,22,0,96,3,22,0,97,3,22,0,98,3,56,0,99,3,1,0,39,0,0,0,238,0,67,0,19,0,25,0,81,0,25,0,100,3,22,0,101,3,22,0,102,3,0,0,33,0,4,0,103,3,22,0,104,3,38,0,231,0,
4,0,145,1,7,0,105,3,1,0,31,0,12,0,106,3,0,0,95,1,1,0,107,3,1,0,108,3,1,0,109,3,1,0,110,3,166,0,111,3,165,0,80,0,167,0,4,0,7,0,112,3,7,0,113,3,2,0,31,0,2,0,114,3,168,0,14,0,2,0,115,3,2,0,31,0,7,0,227,1,7,0,116,3,7,0,117,3,7,0,118,3,7,0,119,3,167,0,21,1,167,0,120,3,167,0,121,3,7,0,122,3,7,0,123,3,2,0,124,3,
0,0,22,0,129,0,13,0,4,0,31,0,4,0,125,3,4,0,126,3,4,0,127,3,84,0,128,3,84,0,129,3,168,0,130,3,7,0,131,3,7,0,132,3,7,0,133,3,7,0,134,3,2,0,135,3,0,0,22,0,169,0,13,0,4,0,210,0,4,0,136,3,7,0,137,3,7,0,138,3,7,0,139,3,7,0,140,3,7,0,141,3,7,0,142,3,7,0,143,3,2,0,248,0,2,0,31,0,4,0,222,1,7,0,144,3,170,0,19,0,
4,0,145,3,4,0,146,3,4,0,147,3,4,0,148,3,4,0,149,3,4,0,150,3,4,0,151,3,4,0,152,3,7,0,226,1,7,0,153,3,7,0,154,3,7,0,155,3,7,0,156,3,169,0,157,3,7,0,158,3,7,0,159,3,7,0,160,3,7,0,161,3,7,0,162,3,171,0,10,0,4,0,31,0,0,0,2,0,0,0,163,3,0,0,164,3,7,0,165,3,7,0,166,3,7,0,167,3,7,0,168,3,129,0,169,3,11,0,188,0,
172,0,1,0,0,0,170,3,173,0,1,0,0,0,32,0,174,0,6,0,7,0,171,3,7,0,172,3,7,0,173,3,7,0,174,3,7,0,175,3,4,0,125,3,135,0,7,0,2,0,176,3,2,0,125,3,0,0,177,3,0,0,178,3,0,0,79,1,0,0,255,1,174,0,179,3,175,0,5,0,175,0,27,0,175,0,28,0,73,0,254,0,2,0,31,0,0,0,180,3,176,0,16,0,176,0,27,0,176,0,28,0,11,0,22,1,2,0,29,0,
2,0,31,0,0,0,181,3,0,0,182,3,2,0,23,1,52,0,183,3,0,0,184,3,0,0,32,0,7,0,185,3,7,0,186,3,73,0,254,0,7,0,187,3,7,0,188,3,177,0,11,0,177,0,27,0,177,0,28,0,52,0,189,3,0,0,190,3,7,0,191,3,2,0,192,3,2,0,31,0,2,0,29,0,2,0,193,3,7,0,140,1,0,0,2,0,178,0,15,0,52,0,189,3,2,0,194,3,2,0,31,0,2,0,195,3,2,0,196,3,
0,0,190,3,52,0,197,3,0,0,198,3,7,0,199,3,7,0,140,1,7,0,200,3,7,0,201,3,2,0,29,0,2,0,248,0,7,0,139,1,179,0,12,0,52,0,189,3,7,0,92,0,2,0,202,3,2,0,203,3,2,0,31,0,2,0,204,3,2,0,205,3,2,0,18,0,7,0,206,3,7,0,207,3,7,0,208,3,7,0,209,3,180,0,3,0,4,0,31,0,0,0,2,0,22,0,210,3,181,0,6,0,52,0,189,3,4,0,211,3,
4,0,212,3,4,0,172,1,0,0,2,0,0,0,190,3,182,0,6,0,52,0,189,3,4,0,31,0,0,0,213,3,0,0,214,3,0,0,49,0,0,0,190,3,183,0,4,0,52,0,189,3,4,0,31,0,4,0,211,3,0,0,190,3,184,0,4,0,52,0,189,3,4,0,31,0,7,0,215,3,0,0,190,3,185,0,4,0,0,0,31,0,0,0,248,0,0,0,49,0,7,0,218,1,186,0,5,0,52,0,189,3,4,0,31,0,0,0,214,3,
0,0,18,0,0,0,190,3,187,0,6,0,52,0,189,3,4,0,216,3,7,0,48,1,4,0,31,0,0,0,190,3,4,0,179,2,188,0,16,0,52,0,189,3,2,0,29,0,2,0,217,3,4,0,87,1,4,0,88,1,7,0,10,0,7,0,11,0,4,0,31,0,0,0,214,3,0,0,18,0,7,0,218,3,62,0,83,1,4,0,245,0,0,0,84,1,0,0,95,1,0,0,190,3,189,0,4,0,52,0,189,3,4,0,219,3,4,0,220,3,
0,0,190,3,190,0,4,0,52,0,189,3,4,0,219,3,0,0,2,0,0,0,190,3,191,0,6,0,52,0,189,3,7,0,48,1,7,0,221,3,4,0,222,3,2,0,219,3,2,0,223,3,192,0,10,0,52,0,189,3,4,0,31,0,4,0,224,3,4,0,225,3,7,0,226,3,7,0,206,3,7,0,207,3,7,0,208,3,7,0,209,3,0,0,190,3,193,0,14,0,52,0,189,3,52,0,137,0,4,0,29,0,7,0,227,3,7,0,228,3,
7,0,229,3,7,0,230,3,7,0,231,3,7,0,232,3,7,0,233,3,7,0,234,3,7,0,235,3,2,0,31,0,0,0,22,0,194,0,3,0,52,0,189,3,4,0,31,0,4,0,0,2,195,0,5,0,52,0,189,3,4,0,31,0,0,0,2,0,7,0,236,3,0,0,190,3,196,0,24,0,52,0,189,3,0,0,190,3,2,0,237,3,2,0,238,3,0,0,239,3,0,0,240,3,0,0,241,3,0,0,242,3,0,0,243,3,0,0,244,3,
0,0,245,3,0,0,18,0,7,0,246,3,7,0,247,3,7,0,248,3,7,0,249,3,7,0,250,3,7,0,251,3,7,0,252,3,7,0,253,3,7,0,254,3,7,0,255,3,7,0,0,4,7,0,1,4,197,0,5,0,52,0,189,3,0,0,190,3,7,0,2,4,2,0,3,4,2,0,31,0,198,0,8,0,7,0,4,4,7,0,142,3,7,0,5,4,7,0,143,3,7,0,6,4,7,0,7,4,2,0,31,0,2,0,0,2,199,0,10,0,
7,0,4,4,7,0,142,3,7,0,5,4,7,0,143,3,7,0,6,4,7,0,7,4,2,0,31,0,2,0,0,2,0,0,213,3,0,0,18,0,200,0,8,0,7,0,4,4,7,0,142,3,7,0,5,4,7,0,143,3,7,0,6,4,7,0,7,4,2,0,31,0,2,0,0,2,201,0,7,0,52,0,189,3,0,0,190,3,7,0,139,1,7,0,8,4,2,0,31,0,2,0,248,0,0,0,2,0,202,0,10,0,52,0,9,4,7,0,139,1,
2,0,10,4,0,0,11,4,0,0,12,4,7,0,13,4,0,0,14,4,0,0,31,0,0,0,15,4,0,0,179,2,203,0,7,0,150,0,205,2,0,0,16,4,4,0,31,0,4,0,17,4,0,0,18,4,52,0,19,4,52,0,20,4,204,0,3,0,150,0,205,2,4,0,31,0,0,0,2,0,205,0,6,0,150,0,205,2,4,0,31,0,0,0,2,0,0,0,18,4,7,0,236,3,52,0,19,4,206,0,4,0,143,0,21,4,0,0,22,4,
207,0,23,4,0,0,24,4,92,0,17,0,7,0,25,4,7,0,26,4,7,0,140,1,7,0,27,4,0,0,28,4,1,0,29,4,1,0,30,4,1,0,36,1,1,0,37,1,1,0,31,4,0,0,32,4,0,0,33,4,7,0,34,4,7,0,31,1,7,0,35,4,0,0,36,4,0,0,18,0,208,0,8,0,7,0,37,4,7,0,26,4,7,0,140,1,1,0,36,1,0,0,38,4,2,0,32,4,7,0,27,4,0,0,2,0,209,0,22,0,
209,0,27,0,209,0,28,0,2,0,29,0,2,0,39,4,2,0,32,4,2,0,31,0,4,0,40,4,4,0,41,4,0,0,2,0,2,0,42,4,2,0,43,4,2,0,44,4,2,0,45,4,2,0,46,4,2,0,47,4,7,0,48,4,7,0,49,4,208,0,50,4,92,0,73,1,2,0,51,4,2,0,52,4,4,0,53,4,210,0,4,0,7,0,54,4,2,0,39,4,0,0,31,0,0,0,255,1,211,0,4,0,7,0,112,3,7,0,113,3,
7,0,55,4,7,0,168,2,212,0,77,0,25,0,81,0,98,0,122,1,22,0,56,4,213,0,57,4,52,0,58,4,52,0,59,4,52,0,60,4,73,0,254,0,214,0,61,4,130,0,62,4,215,0,63,4,7,0,148,0,7,0,149,0,2,0,29,0,0,0,64,4,0,0,238,0,2,0,65,4,7,0,66,4,7,0,67,4,4,0,68,4,2,0,69,4,2,0,70,4,4,0,31,0,7,0,71,4,7,0,72,4,7,0,73,4,2,0,42,4,
2,0,43,4,2,0,74,4,2,0,75,4,4,0,76,4,4,0,77,4,0,0,78,4,0,0,79,4,0,0,80,4,0,0,81,4,0,0,82,4,0,0,179,2,2,0,83,4,7,0,65,2,7,0,84,4,7,0,16,3,7,0,85,4,7,0,86,4,7,0,87,4,7,0,88,4,7,0,89,4,7,0,90,4,7,0,91,4,4,0,175,3,4,0,92,4,4,0,93,4,4,0,94,4,4,0,35,0,0,0,95,4,216,0,96,4,0,0,97,4,
217,0,98,4,217,0,99,4,217,0,100,4,217,0,101,4,211,0,102,4,4,0,103,4,4,0,104,4,210,0,105,4,210,0,106,4,7,0,192,0,7,0,107,4,7,0,108,4,0,0,109,4,0,0,110,4,0,0,111,4,7,0,112,4,218,0,113,4,0,0,114,4,0,0,115,4,11,0,116,4,219,0,9,0,7,0,112,3,7,0,113,3,2,0,31,0,0,0,29,4,0,0,30,4,7,0,117,4,7,0,118,4,0,0,2,0,215,0,119,4,
215,0,10,0,2,0,120,4,2,0,121,4,4,0,126,3,219,0,189,1,219,0,120,3,219,0,122,4,4,0,31,0,4,0,127,3,84,0,123,4,84,0,124,4,220,0,13,0,4,0,125,4,117,0,126,4,221,0,127,4,221,0,128,4,4,0,129,4,4,0,130,4,22,0,131,4,4,0,132,4,4,0,133,4,222,0,54,0,7,0,134,4,4,0,135,4,0,0,2,0,218,0,15,0,25,0,81,0,98,0,122,1,220,0,136,4,4,0,31,0,
4,0,133,4,130,0,62,4,2,0,70,4,0,0,137,4,0,0,138,4,0,0,2,0,52,0,139,4,0,0,140,4,7,0,141,4,0,0,99,0,11,0,116,4,223,0,12,0,4,0,29,0,4,0,48,1,4,0,31,0,4,0,142,4,4,0,143,4,4,0,144,4,4,0,145,4,4,0,146,4,0,0,147,4,0,0,120,0,11,0,22,1,114,0,190,1,224,0,1,0,0,0,148,4,221,0,7,0,223,0,149,4,4,0,150,4,4,0,151,4,
4,0,152,4,4,0,153,4,225,0,154,4,224,0,155,4,226,0,5,0,10,0,156,4,10,0,157,4,10,0,158,4,10,0,159,4,10,0,160,4,227,0,47,0,227,0,27,0,227,0,28,0,228,0,161,4,229,0,22,1,67,0,162,4,159,0,58,3,230,0,163,4,22,0,164,4,4,0,165,4,0,0,32,0,2,0,166,4,2,0,29,0,2,0,167,4,2,0,168,4,2,0,169,4,2,0,170,4,4,0,172,1,4,0,171,4,4,0,172,4,
4,0,173,4,4,0,94,0,4,0,95,0,7,0,174,4,231,0,175,4,0,0,176,4,4,0,177,4,4,0,178,4,7,0,179,4,7,0,180,4,7,0,181,4,7,0,182,4,7,0,183,4,7,0,184,4,7,0,185,4,7,0,186,4,7,0,187,4,7,0,188,4,7,0,189,4,7,0,190,4,7,0,191,4,7,0,192,4,7,0,193,4,0,0,99,0,0,0,194,4,0,0,195,4,0,0,196,4,0,0,197,4,228,0,6,0,232,0,198,4,
22,0,199,4,2,0,200,4,2,0,172,1,0,0,2,0,0,0,201,4,233,0,22,0,232,0,198,4,234,0,202,4,4,0,172,1,4,0,203,4,7,0,171,3,7,0,172,3,7,0,173,3,7,0,71,2,7,0,204,4,7,0,205,4,7,0,206,4,7,0,207,4,135,0,208,4,135,0,209,4,2,0,210,4,2,0,211,4,2,0,212,4,0,0,49,0,7,0,213,4,7,0,214,4,7,0,215,4,7,0,216,4,235,0,6,0,235,0,27,0,
235,0,28,0,2,0,29,0,2,0,31,0,2,0,217,4,0,0,40,0,236,0,8,0,236,0,27,0,236,0,28,0,2,0,29,0,2,0,31,0,2,0,217,4,0,0,40,0,7,0,35,0,7,0,24,1,237,0,45,0,237,0,27,0,237,0,28,0,2,0,29,0,2,0,31,0,2,0,217,4,2,0,218,4,2,0,219,4,2,0,220,4,7,0,221,4,7,0,88,1,7,0,222,4,4,0,223,4,4,0,224,4,4,0,225,4,7,0,226,4,
7,0,227,4,7,0,228,4,7,0,229,4,7,0,230,4,7,0,231,4,7,0,232,4,7,0,233,4,7,0,234,4,7,0,235,4,7,0,236,4,0,0,2,0,7,0,237,4,7,0,238,4,2,0,239,4,2,0,240,4,2,0,241,4,2,0,242,4,2,0,243,4,2,0,244,4,2,0,245,4,2,0,246,4,2,0,0,2,2,0,247,4,2,0,248,4,2,0,249,4,0,0,250,4,0,0,251,4,7,0,252,4,238,0,253,4,67,0,85,3,
239,0,16,0,239,0,27,0,239,0,28,0,2,0,29,0,2,0,31,0,2,0,217,4,2,0,218,4,7,0,254,4,7,0,255,4,7,0,222,1,7,0,71,4,7,0,0,5,7,0,208,1,7,0,1,5,7,0,232,4,7,0,2,5,7,0,222,4,240,0,14,0,0,0,3,5,2,0,4,5,2,0,5,5,2,0,6,5,0,0,22,0,241,0,7,5,242,0,8,5,166,0,9,5,11,0,75,0,4,0,10,5,4,0,11,5,10,0,12,5,
0,0,13,5,0,0,148,4,243,0,185,0,244,0,14,5,245,0,15,5,245,0,16,5,11,0,17,5,67,0,18,5,67,0,19,5,67,0,20,5,246,0,21,5,246,0,22,5,246,0,23,5,246,0,24,5,246,0,25,5,246,0,26,5,246,0,27,5,246,0,28,5,246,0,29,5,246,0,30,5,246,0,31,5,246,0,32,5,246,0,33,5,52,0,34,5,159,0,58,3,7,0,35,5,7,0,36,5,7,0,37,5,7,0,38,5,7,0,39,5,
7,0,40,5,4,0,41,5,7,0,42,5,7,0,43,5,7,0,252,4,7,0,44,5,7,0,45,5,7,0,46,5,4,0,47,5,4,0,48,5,4,0,49,5,4,0,50,5,4,0,51,5,7,0,52,5,7,0,92,1,4,0,53,5,7,0,54,5,4,0,55,5,4,0,56,5,7,0,57,5,4,0,58,5,4,0,59,5,4,0,60,5,4,0,172,1,7,0,12,3,4,0,61,5,2,0,29,0,0,0,111,4,7,0,71,2,7,0,62,5,
4,0,178,4,7,0,63,5,7,0,64,5,4,0,65,5,7,0,66,5,7,0,67,5,7,0,68,5,7,0,69,5,7,0,70,5,7,0,71,5,7,0,72,5,7,0,73,5,7,0,74,5,4,0,75,5,4,0,76,5,0,0,77,5,7,0,78,5,4,0,79,5,4,0,80,5,4,0,81,5,7,0,205,4,7,0,82,5,7,0,83,5,7,0,84,5,7,0,85,5,4,0,86,5,2,0,87,5,0,0,88,5,7,0,89,5,0,0,90,5,
7,0,91,5,7,0,92,5,4,0,93,5,7,0,94,5,7,0,95,5,7,0,96,5,4,0,97,5,4,0,98,5,4,0,99,5,2,0,100,5,0,0,101,5,4,0,102,5,4,0,103,5,7,0,104,5,7,0,105,5,7,0,106,5,7,0,107,5,7,0,108,5,7,0,109,5,4,0,110,5,4,0,111,5,7,0,112,5,7,0,113,5,7,0,114,5,7,0,115,5,4,0,116,5,4,0,117,5,0,0,118,5,0,0,119,5,0,0,44,3,
7,0,120,5,4,0,121,5,7,0,122,5,4,0,123,5,2,0,124,5,0,0,125,5,4,0,126,5,4,0,127,5,4,0,128,5,4,0,129,5,4,0,130,5,4,0,131,5,4,0,132,5,4,0,133,5,4,0,134,5,0,0,135,5,0,0,136,5,0,0,137,5,0,0,138,5,0,0,139,5,0,0,201,4,2,0,140,5,0,0,141,5,0,0,142,5,7,0,13,3,7,0,143,5,7,0,144,5,7,0,145,5,7,0,24,3,7,0,146,5,
4,0,147,5,4,0,148,5,7,0,149,5,7,0,150,5,7,0,151,5,7,0,152,5,135,0,153,5,7,0,154,5,7,0,155,5,7,0,156,5,7,0,157,5,0,0,158,5,0,0,159,5,0,0,160,5,0,0,161,5,0,0,162,5,0,0,163,5,0,0,164,5,0,0,165,5,0,0,166,5,0,0,167,5,0,0,168,5,0,0,169,5,0,0,170,5,0,0,171,5,7,0,172,5,4,0,173,5,7,0,174,5,0,0,175,5,0,0,176,5,
4,0,177,5,0,0,178,5,230,0,179,5,22,0,180,5,4,0,181,5,4,0,182,5,0,0,183,5,0,0,184,5,247,0,31,0,244,0,14,5,248,0,185,5,234,0,202,4,231,0,186,5,7,0,187,5,4,0,188,5,7,0,189,5,7,0,190,5,7,0,191,5,7,0,192,5,0,0,120,0,7,0,76,2,7,0,96,0,7,0,193,5,7,0,167,3,7,0,194,5,7,0,195,5,7,0,196,5,4,0,197,5,7,0,198,5,7,0,199,5,
0,0,99,0,0,0,194,4,0,0,77,5,2,0,200,5,2,0,29,0,2,0,201,5,2,0,209,2,2,0,202,5,2,0,203,5,4,0,172,1,249,0,12,0,244,0,14,5,248,0,185,5,7,0,187,5,4,0,188,5,7,0,195,5,4,0,172,1,4,0,197,5,2,0,29,0,0,0,252,0,7,0,189,5,2,0,204,5,0,0,253,1,250,0,14,0,250,0,27,0,250,0,28,0,0,0,32,0,4,0,172,1,4,0,205,5,2,0,206,5,
0,0,95,1,4,0,207,5,4,0,208,5,4,0,209,5,4,0,210,5,0,0,99,0,67,0,85,3,251,0,211,5,252,0,5,0,252,0,27,0,252,0,28,0,155,0,212,5,2,0,213,5,0,0,22,0,253,0,8,0,22,0,214,5,4,0,248,0,4,0,215,5,4,0,172,1,7,0,216,5,7,0,217,5,7,0,218,5,22,0,219,5,254,0,12,0,7,0,112,3,7,0,113,3,7,0,220,5,7,0,174,2,7,0,46,1,7,0,35,1,
4,0,31,0,7,0,221,5,7,0,222,5,7,0,223,5,7,0,224,5,0,0,99,0,255,0,1,0,4,0,225,5,0,1,7,0,0,1,27,0,0,1,28,0,0,0,226,5,7,0,227,5,7,0,228,5,2,0,31,0,0,0,22,0,1,1,6,0,1,1,27,0,1,1,28,0,22,0,171,2,0,0,226,5,2,0,31,0,0,0,22,0,2,1,10,0,92,0,229,5,7,0,174,2,7,0,46,1,4,0,230,5,4,0,31,0,7,0,221,5,
7,0,222,5,7,0,223,5,7,0,224,5,0,0,2,0,3,1,4,0,2,1,231,5,4,0,232,5,2,0,31,0,0,0,49,0,4,1,9,0,0,0,233,5,7,0,234,5,4,0,235,5,4,0,236,5,4,0,237,5,4,0,238,5,4,0,95,3,5,1,239,5,11,0,188,0,5,1,27,0,5,1,27,0,5,1,28,0,254,0,92,0,255,0,240,5,4,0,241,5,4,0,242,5,2,0,243,5,2,0,31,0,2,0,49,0,8,0,244,5,
0,0,245,5,4,0,39,4,2,0,246,5,7,0,11,2,7,0,12,2,7,0,247,5,7,0,248,5,7,0,249,5,7,0,250,5,4,0,251,5,0,0,252,5,6,1,253,5,11,0,98,1,7,0,254,5,3,1,255,5,4,1,80,0,11,0,0,6,7,1,2,0,4,0,1,6,4,0,2,6,8,1,7,0,8,1,27,0,8,1,28,0,22,0,3,6,4,0,4,6,2,0,31,0,2,0,5,6,7,1,80,0,9,1,6,0,9,1,27,0,
9,1,28,0,0,0,6,6,2,0,31,0,2,0,7,6,0,0,2,0,10,1,2,0,4,0,68,0,0,0,2,0,11,1,36,0,11,1,27,0,11,1,28,0,22,0,8,6,8,1,9,6,2,0,31,0,2,0,10,6,7,0,227,5,7,0,228,5,0,0,11,6,2,0,243,5,2,0,12,6,52,0,136,0,7,0,13,6,0,0,14,6,2,0,15,6,2,0,16,6,7,0,17,6,7,0,18,6,0,0,19,6,4,0,20,6,7,0,21,6,
2,0,22,6,2,0,23,6,7,0,24,6,7,0,25,6,0,0,120,0,22,0,26,6,4,0,27,6,0,0,99,0,7,0,28,6,7,0,29,6,7,0,30,6,7,0,31,6,7,0,32,6,0,0,77,5,10,1,80,0,12,1,14,0,11,0,33,6,2,0,34,6,2,0,35,6,2,0,36,6,0,0,95,1,4,0,37,6,4,0,38,6,7,0,254,5,7,0,247,5,7,0,39,6,7,0,40,6,4,0,41,6,4,0,42,6,0,0,2,0,
13,1,6,0,7,0,96,0,7,0,43,6,7,0,207,2,0,0,120,0,4,0,83,4,0,0,2,0,14,1,32,0,25,0,81,0,98,0,122,1,22,0,181,2,4,0,31,0,4,0,44,6,7,0,45,6,7,0,46,6,22,0,47,6,22,0,131,4,7,0,48,6,7,0,49,6,7,0,50,6,4,0,51,6,4,0,10,6,2,0,22,6,2,0,23,6,7,0,24,6,7,0,25,6,7,0,52,6,130,0,62,4,2,0,70,4,2,0,151,4,
2,0,53,6,0,0,111,4,4,0,54,6,4,0,115,3,2,0,55,6,2,0,56,6,4,0,57,6,4,0,132,4,13,1,58,6,12,1,80,0,15,1,9,0,15,1,27,0,15,1,28,0,4,0,29,0,4,0,248,0,0,0,33,0,2,0,31,0,2,0,23,1,0,0,32,0,0,0,59,6,16,1,19,0,15,1,60,6,130,0,37,2,0,0,61,6,0,0,62,6,0,0,63,6,4,0,12,6,4,0,31,0,7,0,64,6,7,0,65,6,
7,0,66,6,7,0,67,6,7,0,76,5,7,0,68,6,2,0,69,6,0,0,49,0,4,0,14,0,4,0,70,6,4,0,225,4,129,0,71,6,17,1,10,0,15,1,60,6,130,0,37,2,0,0,61,6,0,0,62,6,4,0,12,6,4,0,31,0,4,0,207,1,4,0,70,6,2,0,29,0,0,0,22,0,18,1,12,0,15,1,60,6,130,0,37,2,0,0,61,6,0,0,62,6,0,0,63,6,4,0,12,6,4,0,31,0,7,0,72,6,
4,0,243,5,4,0,70,6,0,0,2,0,129,0,73,6,19,1,6,0,0,0,32,0,20,1,74,6,4,0,75,6,4,0,76,6,4,0,77,6,4,0,78,6,20,1,14,0,15,1,60,6,130,0,37,2,0,0,61,6,4,0,70,6,4,0,31,0,4,0,48,1,7,0,79,6,4,0,248,0,4,0,24,1,4,0,25,1,0,0,2,0,19,1,122,4,4,0,121,4,4,0,80,6,21,1,12,0,15,1,60,6,130,0,37,2,0,0,61,6,
0,0,62,6,4,0,12,6,4,0,31,0,7,0,81,6,0,0,82,6,0,0,18,0,4,0,70,6,0,0,120,0,129,0,71,6,22,1,13,0,15,1,60,6,130,0,37,2,0,0,61,6,0,0,62,6,0,0,63,6,4,0,12,6,4,0,31,0,7,0,64,6,0,0,82,6,0,0,18,0,4,0,70,6,7,0,83,6,129,0,71,6,23,1,11,0,15,1,60,6,52,0,84,6,130,0,37,2,0,0,61,6,4,0,12,6,4,0,31,0,
4,0,243,5,7,0,85,6,4,0,86,6,4,0,70,6,130,0,87,6,24,1,17,0,15,1,60,6,52,0,84,6,130,0,37,2,4,0,88,6,4,0,31,0,7,0,2,4,7,0,41,5,7,0,89,6,7,0,90,6,7,0,91,6,0,0,2,0,4,0,225,4,4,0,12,6,0,0,61,6,0,0,62,6,4,0,92,6,4,0,70,6,25,1,24,0,15,1,60,6,130,0,37,2,0,0,61,6,4,0,12,6,0,0,62,6,4,0,70,6,
7,0,94,0,7,0,95,0,7,0,93,6,7,0,93,0,2,0,31,0,2,0,248,0,2,0,94,6,2,0,95,6,7,0,96,6,7,0,97,6,2,0,98,6,0,0,22,0,52,0,84,6,7,0,99,6,7,0,100,6,0,0,101,6,7,0,102,6,7,0,103,6,26,1,11,0,15,1,60,6,52,0,84,6,130,0,37,2,0,0,61,6,0,0,62,6,0,0,63,6,4,0,12,6,4,0,31,0,7,0,46,1,4,0,70,6,27,1,104,6,
28,1,19,0,15,1,60,6,130,0,37,2,0,0,61,6,4,0,12,6,4,0,31,0,4,0,70,6,7,0,105,6,7,0,106,6,7,0,107,6,7,0,108,6,7,0,109,6,7,0,110,6,4,0,225,4,4,0,14,0,4,0,248,0,0,0,2,0,7,0,111,6,7,0,112,6,7,0,113,6,29,1,8,0,0,0,32,0,30,1,114,6,4,0,115,6,4,0,116,6,7,0,27,4,7,0,18,6,4,0,39,4,4,0,31,0,30,1,10,0,
15,1,60,6,130,0,37,2,0,0,61,6,4,0,12,6,4,0,31,0,4,0,70,6,4,0,117,6,29,1,122,4,4,0,121,4,4,0,80,6,31,1,9,0,15,1,60,6,52,0,84,6,130,0,37,2,0,0,61,6,0,0,62,6,4,0,12,6,4,0,31,0,4,0,70,6,0,0,2,0,32,1,18,0,15,1,60,6,52,0,84,6,130,0,37,2,0,0,190,3,0,0,61,6,0,0,62,6,0,0,63,6,4,0,12,6,4,0,70,6,
0,0,2,0,4,0,31,0,0,0,118,6,0,0,133,1,7,0,119,6,7,0,120,6,7,0,219,1,7,0,121,6,129,0,122,6,33,1,13,0,15,1,60,6,130,0,37,2,0,0,61,6,0,0,62,6,4,0,12,6,4,0,31,0,7,0,64,6,2,0,248,0,2,0,14,0,4,0,70,6,7,0,93,0,7,0,154,2,7,0,204,1,34,1,19,0,15,1,60,6,130,0,37,2,0,0,61,6,0,0,62,6,0,0,63,6,4,0,12,6,
4,0,31,0,7,0,148,0,7,0,123,6,7,0,30,6,7,0,89,6,7,0,90,6,7,0,91,6,4,0,225,4,4,0,248,0,4,0,124,6,4,0,125,6,4,0,70,6,0,0,2,0,35,1,12,0,15,1,60,6,130,0,37,2,0,0,61,6,0,0,62,6,0,0,63,6,4,0,12,6,4,0,31,0,7,0,64,6,4,0,14,0,4,0,70,6,0,0,120,0,129,0,71,6,36,1,7,0,15,1,60,6,2,0,162,1,2,0,126,6,
4,0,179,2,52,0,84,6,7,0,127,6,0,0,63,6,37,1,14,0,15,1,60,6,130,0,37,2,0,0,61,6,0,0,62,6,4,0,12,6,4,0,31,0,4,0,70,6,4,0,172,1,4,0,128,6,7,0,204,1,7,0,48,1,7,0,129,6,7,0,130,6,7,0,131,6,38,1,16,0,15,1,60,6,52,0,84,6,130,0,37,2,0,0,61,6,0,0,62,6,0,0,63,6,4,0,12,6,4,0,70,6,4,0,31,0,4,0,248,0,
7,0,64,6,7,0,27,4,7,0,69,2,4,0,29,0,129,0,71,6,135,0,132,6,39,1,17,0,15,1,60,6,130,0,37,2,0,0,61,6,0,0,62,6,0,0,63,6,4,0,12,6,4,0,31,0,7,0,133,6,7,0,250,5,7,0,134,6,7,0,135,6,7,0,136,6,4,0,70,6,2,0,137,6,2,0,248,0,7,0,138,6,0,0,2,0,40,1,12,0,15,1,60,6,0,0,101,6,130,0,37,2,0,0,61,6,0,0,63,6,
4,0,12,6,4,0,31,0,7,0,139,6,4,0,70,6,7,0,140,6,7,0,141,6,52,0,84,6,41,1,12,0,15,1,60,6,0,0,101,6,130,0,37,2,0,0,61,6,0,0,63,6,4,0,12,6,4,0,31,0,7,0,139,6,4,0,70,6,2,0,142,6,2,0,192,3,7,0,143,6,42,1,42,0,15,1,60,6,3,0,144,6,0,0,145,6,0,0,146,6,2,0,147,6,2,0,148,6,52,0,149,6,52,0,150,6,52,0,151,6,
67,0,152,6,130,0,153,6,0,0,154,6,0,0,155,6,0,0,63,6,7,0,156,6,7,0,157,6,7,0,158,6,7,0,159,6,7,0,160,6,7,0,18,6,2,0,243,5,0,0,161,6,0,0,162,6,0,0,163,6,0,0,164,6,0,0,165,6,0,0,255,1,7,0,166,6,7,0,167,6,7,0,168,6,7,0,169,6,4,0,170,6,4,0,172,1,7,0,171,6,0,0,172,6,0,0,173,6,2,0,174,6,0,0,175,6,0,0,176,6,
0,0,111,4,43,1,8,3,44,1,177,6,45,1,20,0,15,1,60,6,52,0,9,4,52,0,178,6,130,0,37,2,0,0,61,6,0,0,63,6,4,0,12,6,4,0,31,0,4,0,70,6,7,0,179,6,2,0,180,6,0,0,181,6,0,0,182,6,7,0,183,6,0,0,184,6,0,0,185,6,0,0,22,0,7,0,186,6,4,0,187,6,46,1,104,6,47,1,14,0,15,1,60,6,130,0,37,2,0,0,61,6,0,0,63,6,4,0,12,6,
4,0,31,0,4,0,248,0,4,0,39,4,7,0,243,5,7,0,46,1,4,0,188,6,4,0,70,6,4,0,189,6,0,0,2,0,156,0,8,0,7,0,212,2,7,0,190,6,7,0,191,6,7,0,192,6,7,0,208,2,7,0,193,6,4,0,194,6,4,0,195,6,48,1,3,0,12,0,29,0,0,0,18,0,4,0,31,0,49,1,3,0,48,1,7,0,220,0,136,4,50,1,54,0,51,1,2,0,48,1,7,0,52,1,196,6,53,1,4,0,
4,0,197,6,4,0,31,0,12,0,29,0,0,0,18,0,54,1,4,0,4,0,253,4,53,1,198,6,4,0,45,1,4,0,31,0,55,1,5,0,55,1,27,0,55,1,28,0,0,0,199,6,3,0,31,0,0,0,22,0,56,1,8,0,56,1,27,0,56,1,28,0,57,1,136,0,0,0,4,0,12,0,29,0,0,0,47,2,7,0,96,0,4,0,31,0,58,1,17,0,56,1,7,0,54,1,200,6,12,0,20,6,0,0,18,0,7,0,18,6,
22,0,201,6,4,0,202,6,0,0,99,0,52,0,136,0,0,0,203,6,7,0,119,6,7,0,204,6,7,0,29,6,7,0,30,6,0,0,77,5,0,0,205,6,59,1,54,0,57,1,5,0,56,1,7,0,22,0,102,3,12,0,106,3,0,0,47,2,60,1,54,0,61,1,10,0,7,0,18,6,12,0,248,0,1,0,31,0,1,0,206,6,0,0,255,1,2,0,207,6,2,0,208,6,7,0,209,6,7,0,210,6,0,0,99,0,52,1,20,0,
25,0,81,0,98,0,122,1,48,1,211,6,4,0,212,6,0,0,2,0,57,1,213,6,221,0,214,6,117,0,126,4,4,0,133,4,0,0,99,0,56,1,215,6,130,0,216,6,2,0,217,6,0,0,218,6,4,0,31,0,22,0,131,4,4,0,132,4,0,0,252,5,61,1,219,6,62,1,54,0,149,0,13,0,242,0,220,6,4,0,221,6,4,0,8,6,4,0,48,1,4,0,24,1,0,0,222,6,0,0,223,6,2,0,224,6,4,0,225,6,
2,0,226,6,2,0,227,6,2,0,145,1,2,0,31,0,63,1,3,0,63,1,27,0,63,1,28,0,64,1,228,6,65,1,4,0,65,1,27,0,65,1,28,0,0,0,32,0,0,0,178,2,66,1,6,0,66,1,27,0,66,1,28,0,36,0,83,0,4,0,227,6,4,0,229,6,0,0,178,2,67,1,4,0,67,1,27,0,67,1,28,0,0,0,32,0,68,1,230,6,69,1,4,0,4,0,231,6,4,0,179,2,4,0,232,6,4,0,233,6,
70,1,11,0,70,1,27,0,70,1,28,0,69,1,80,0,4,0,229,6,4,0,234,6,4,0,235,6,0,0,236,6,0,0,237,6,2,0,238,6,7,0,239,6,0,0,240,6,148,0,43,0,25,0,81,0,98,0,122,1,0,0,82,0,71,1,8,3,246,0,241,6,22,0,242,6,68,1,243,6,22,0,244,6,2,0,245,6,2,0,246,6,4,0,31,0,2,0,209,2,2,0,29,0,4,0,247,6,4,0,248,6,2,0,249,6,2,0,250,6,
2,0,251,6,2,0,252,6,2,0,253,6,0,0,253,1,36,0,83,0,22,0,254,6,38,0,231,0,4,0,255,6,4,0,234,6,4,0,235,6,0,0,236,6,0,0,237,6,2,0,238,6,7,0,239,6,7,0,0,7,7,0,1,7,173,0,2,7,0,0,3,7,0,0,179,2,0,0,4,7,0,0,5,7,4,0,6,7,22,0,7,7,22,0,8,7,72,1,9,7,73,1,54,0,74,1,6,0,52,0,198,1,2,0,10,7,2,0,11,7,
2,0,29,0,2,0,31,0,0,0,6,6,75,1,21,0,75,1,27,0,75,1,28,0,208,0,50,4,92,0,73,1,84,0,12,7,84,0,13,7,2,0,10,7,2,0,11,7,2,0,14,7,2,0,38,1,2,0,28,4,2,0,15,7,2,0,31,0,0,0,40,0,7,0,5,4,7,0,143,3,4,0,16,7,7,0,17,7,7,0,18,7,7,0,65,1,74,1,71,1,73,0,7,0,25,0,81,0,22,0,19,7,84,0,125,3,2,0,10,7,
2,0,20,7,2,0,21,7,0,0,49,0,76,1,15,0,76,1,27,0,76,1,28,0,7,0,175,3,7,0,65,1,2,0,29,0,0,0,95,1,2,0,22,7,2,0,31,0,4,0,23,7,4,0,146,4,11,0,22,1,0,0,32,0,0,0,24,7,7,0,25,7,7,0,26,7,214,0,15,0,25,0,81,0,98,0,122,1,76,1,27,7,0,0,28,7,4,0,29,7,0,0,2,0,22,0,30,7,73,0,254,0,25,0,31,7,4,0,224,4,
2,0,31,0,0,0,29,0,0,0,252,2,7,0,192,0,4,0,32,7,77,1,30,0,25,0,81,0,98,0,122,1,2,0,40,4,2,0,41,4,2,0,33,7,2,0,31,0,2,0,34,7,2,0,35,7,2,0,36,7,0,0,191,2,0,0,37,7,0,0,38,7,0,0,39,7,4,0,40,7,7,0,41,7,7,0,42,7,7,0,43,7,7,0,44,7,7,0,45,7,7,0,46,7,208,0,47,7,73,0,254,0,214,0,61,4,6,1,253,5,
0,0,24,7,22,0,131,4,4,0,132,4,0,0,33,0,78,1,48,7,11,0,116,4,79,1,11,0,79,1,27,0,79,1,28,0,52,0,84,6,79,1,49,7,4,0,50,7,2,0,31,0,2,0,51,7,2,0,52,7,2,0,53,7,2,0,54,7,0,0,95,1,80,1,10,0,80,1,27,0,80,1,28,0,67,0,92,3,11,0,75,0,2,0,31,0,2,0,55,7,0,0,2,0,22,0,56,7,2,0,54,7,2,0,191,2,81,1,2,0,
4,0,57,7,4,0,255,1,82,1,5,0,82,1,27,0,82,1,28,0,0,0,32,0,4,0,31,0,4,0,29,0,83,1,3,0,83,1,27,0,83,1,28,0,0,0,32,0,84,1,1,0,0,0,32,0,166,0,29,0,166,0,27,0,166,0,28,0,0,0,32,0,2,0,31,0,0,0,22,0,22,0,58,7,85,1,59,7,79,1,60,7,22,0,56,7,80,1,155,1,4,0,61,7,4,0,62,7,7,0,63,7,2,0,64,7,2,0,65,7,
4,0,66,7,4,0,67,7,130,0,68,7,86,1,69,7,23,0,70,7,23,0,74,0,253,0,71,7,81,1,72,7,22,0,73,7,82,1,74,7,22,0,75,7,83,1,76,7,79,1,77,7,56,0,78,7,87,1,42,0,25,0,81,0,98,0,122,1,2,0,29,0,2,0,31,0,4,0,248,0,7,0,171,3,7,0,172,3,7,0,173,3,7,0,167,3,7,0,79,7,7,0,165,3,7,0,27,4,7,0,80,7,7,0,81,7,2,0,82,7,
2,0,83,7,7,0,84,7,7,0,85,7,7,0,86,7,7,0,87,7,7,0,88,7,2,0,89,7,2,0,90,7,7,0,224,2,7,0,225,2,7,0,91,7,7,0,92,7,7,0,93,7,4,0,94,7,7,0,95,7,7,0,96,7,7,0,97,7,7,0,98,7,7,0,99,7,7,0,100,7,7,0,101,7,7,0,102,7,38,0,231,0,88,1,103,7,73,0,254,0,7,0,104,7,7,0,252,2,89,1,35,0,25,0,81,0,98,0,122,1,
0,0,29,0,0,0,31,0,0,0,105,7,0,0,106,7,0,0,107,7,0,0,11,1,7,0,108,7,7,0,109,7,7,0,219,1,7,0,224,2,7,0,225,2,7,0,110,7,7,0,111,7,7,0,112,7,7,0,113,7,4,0,114,7,4,0,115,7,4,0,116,7,4,0,117,7,7,0,118,7,7,0,119,7,7,0,120,7,7,0,121,7,7,0,122,7,7,0,123,7,7,0,124,7,7,0,125,7,7,0,126,7,7,0,127,7,4,0,128,7,
67,0,129,7,7,0,130,7,0,0,120,0,90,1,7,0,7,0,131,7,7,0,106,7,7,0,132,7,7,0,105,7,7,0,218,6,7,0,133,7,7,0,134,7,91,1,15,0,7,0,135,7,4,0,136,7,4,0,48,1,7,0,137,7,7,0,138,7,7,0,139,7,7,0,140,7,7,0,141,7,7,0,142,7,7,0,143,7,7,0,144,7,7,0,145,7,7,0,146,7,7,0,147,7,7,0,148,7,92,1,6,0,246,0,149,7,0,0,22,1,
4,0,150,7,0,0,191,1,0,0,151,7,0,0,49,0,93,1,14,0,4,0,31,0,4,0,152,7,4,0,29,0,4,0,153,7,4,0,154,7,4,0,155,7,4,0,156,7,4,0,157,7,0,0,158,7,92,1,159,7,92,1,160,7,92,1,161,7,90,1,162,7,91,1,163,7,94,1,6,0,7,0,164,7,7,0,165,7,7,0,166,7,7,0,167,7,7,0,168,7,7,0,169,7,95,1,4,0,7,0,164,7,7,0,165,7,7,0,166,7,
7,0,167,7,96,1,4,0,7,0,170,7,7,0,171,7,7,0,172,7,7,0,173,7,97,1,1,0,1,0,168,7,98,1,2,0,4,0,2,4,4,0,207,1,99,1,12,0,4,0,149,0,4,0,174,7,4,0,175,7,4,0,176,7,98,1,177,7,94,1,178,7,95,1,179,7,96,1,180,7,97,1,181,7,0,0,2,0,4,0,182,7,11,0,183,7,100,1,5,0,4,0,140,5,0,0,184,7,0,0,185,7,0,0,40,0,99,1,186,7,
101,1,7,0,101,1,27,0,101,1,28,0,0,0,32,0,4,0,29,0,7,0,1,1,4,0,172,1,4,0,58,2,102,1,2,0,101,1,60,6,135,0,187,7,103,1,4,0,101,1,60,6,129,0,21,1,4,0,172,1,0,0,2,0,104,1,6,0,101,1,60,6,129,0,21,1,4,0,172,1,7,0,188,7,7,0,189,7,0,0,2,0,105,1,4,0,101,1,60,6,135,0,187,7,7,0,190,7,7,0,191,7,106,1,6,0,101,1,60,6,
129,0,21,1,4,0,172,1,7,0,190,7,7,0,191,7,0,0,2,0,107,1,8,0,101,1,60,6,129,0,21,1,4,0,172,1,7,0,190,7,7,0,191,7,7,0,188,7,7,0,189,7,0,0,2,0,108,1,5,0,101,1,60,6,52,0,9,4,135,0,187,7,7,0,190,7,7,0,191,7,109,1,7,0,101,1,60,6,52,0,9,4,129,0,21,1,4,0,172,1,7,0,190,7,7,0,191,7,0,0,2,0,110,1,9,0,101,1,60,6,
52,0,9,4,129,0,21,1,4,0,172,1,7,0,190,7,7,0,191,7,7,0,188,7,7,0,189,7,0,0,2,0,111,1,6,0,101,1,60,6,7,0,192,7,7,0,193,7,135,0,187,7,7,0,190,7,7,0,191,7,112,1,6,0,101,1,60,6,129,0,21,1,4,0,172,1,7,0,192,7,7,0,193,7,0,0,2,0,113,1,8,0,101,1,60,6,129,0,21,1,4,0,172,1,0,0,2,0,7,0,192,7,7,0,193,7,7,0,194,7,
7,0,195,7,114,1,6,0,101,1,60,6,135,0,187,7,7,0,35,4,7,0,31,1,4,0,225,4,0,0,2,0,115,1,6,0,101,1,60,6,129,0,21,1,4,0,172,1,7,0,35,4,7,0,31,1,4,0,225,4,116,1,5,0,101,1,60,6,7,0,35,4,7,0,31,1,4,0,172,1,4,0,225,4,117,1,4,0,101,1,60,6,135,0,187,7,7,0,196,7,7,0,113,6,118,1,6,0,101,1,60,6,129,0,21,1,4,0,172,1,
7,0,196,7,7,0,113,6,0,0,2,0,119,1,8,0,101,1,60,6,129,0,21,1,4,0,172,1,0,0,2,0,7,0,196,7,7,0,113,6,7,0,194,7,7,0,195,7,120,1,2,0,101,1,60,6,135,0,187,7,121,1,4,0,101,1,60,6,129,0,21,1,4,0,172,1,0,0,2,0,122,1,6,0,101,1,60,6,129,0,21,1,4,0,172,1,7,0,194,7,7,0,195,7,0,0,2,0,123,1,4,0,101,1,60,6,135,0,187,7,
4,0,172,1,4,0,197,7,124,1,4,0,101,1,60,6,129,0,21,1,4,0,172,1,4,0,197,7,125,1,6,0,101,1,60,6,129,0,21,1,4,0,172,1,7,0,188,7,7,0,189,7,4,0,197,7,126,1,3,0,101,1,60,6,7,0,198,7,0,0,2,0,127,1,3,0,101,1,60,6,7,0,199,7,0,0,2,0,128,1,5,0,101,1,60,6,7,0,200,7,7,0,31,1,7,0,47,1,0,0,2,0,129,1,5,0,101,1,60,6,
7,0,31,1,7,0,92,1,4,0,201,7,4,0,172,1,130,1,7,0,101,1,60,6,7,0,202,7,7,0,31,1,7,0,143,6,4,0,201,7,4,0,225,4,0,0,120,0,131,1,7,0,101,1,60,6,7,0,202,7,7,0,31,1,7,0,143,6,4,0,201,7,4,0,225,4,0,0,120,0,132,1,3,0,101,1,60,6,7,0,203,7,0,0,2,0,133,1,3,0,101,1,60,6,7,0,204,7,0,0,2,0,134,1,3,0,101,1,60,6,
7,0,199,7,0,0,2,0,135,1,3,0,101,1,60,6,7,0,48,1,0,0,2,0,136,1,7,0,101,1,60,6,4,0,172,1,4,0,205,7,7,0,203,7,4,0,206,7,4,0,207,7,4,0,208,7,137,1,5,0,101,1,60,6,7,0,87,1,7,0,88,1,7,0,112,3,7,0,113,3,138,1,9,0,101,1,60,6,4,0,200,2,7,0,209,7,7,0,210,7,7,0,143,6,7,0,211,7,7,0,212,7,7,0,213,7,0,0,2,0,
139,1,3,0,101,1,60,6,7,0,214,7,0,0,2,0,140,1,5,0,101,1,60,6,7,0,194,7,7,0,195,7,7,0,215,7,0,0,2,0,251,0,45,0,25,0,81,0,98,0,122,1,7,0,171,3,7,0,172,3,7,0,173,3,7,0,71,2,7,0,243,5,4,0,216,7,7,0,217,7,4,0,31,0,4,0,218,7,4,0,219,7,4,0,205,7,7,0,220,7,7,0,196,7,7,0,113,6,7,0,221,7,7,0,222,7,4,0,223,7,
2,0,224,7,2,0,225,7,2,0,226,7,2,0,227,7,2,0,228,7,2,0,229,7,4,0,230,7,4,0,231,7,7,0,232,7,2,0,233,7,2,0,89,7,2,0,90,7,0,0,22,0,2,0,234,7,2,0,235,7,2,0,236,7,2,0,237,7,2,0,238,7,2,0,239,7,4,0,240,7,133,0,241,7,88,1,103,7,22,0,242,7,22,0,243,7,22,0,244,7,22,0,245,7,141,1,2,0,141,1,27,0,141,1,28,0,142,1,3,0,
142,1,27,0,142,1,28,0,11,0,22,1,22,0,2,0,11,0,246,7,11,0,247,7,143,1,1,0,10,0,248,7,144,1,11,0,25,0,81,0,98,0,122,1,22,0,249,7,4,0,250,7,4,0,251,7,4,0,24,1,4,0,25,1,4,0,31,0,0,0,2,0,11,0,75,0,143,1,80,0,145,1,7,0,4,0,21,0,4,0,29,0,25,0,55,1,0,0,252,7,0,0,253,7,7,0,254,7,7,0,255,7,146,1,3,0,7,0,0,8,
7,0,55,4,4,0,31,0,147,1,5,0,92,0,229,5,0,0,2,0,4,0,1,8,146,1,2,8,145,1,3,8,148,1,9,0,148,1,27,0,148,1,28,0,2,0,31,0,0,0,4,8,0,0,5,8,4,0,6,8,147,1,92,0,145,1,3,8,147,1,7,8,149,1,7,0,149,1,27,0,149,1,28,0,7,0,22,1,4,0,8,8,4,0,186,2,0,0,31,0,0,0,47,2,150,1,14,0,150,1,27,0,150,1,28,0,0,0,32,0,
22,0,9,8,22,0,10,8,148,1,11,8,147,1,12,8,7,0,71,2,0,0,58,2,0,0,13,8,0,0,219,1,0,0,47,2,0,0,31,0,0,0,14,8,151,1,6,0,148,0,203,2,149,0,15,8,0,0,16,8,0,0,17,8,4,0,18,8,4,0,19,8,152,1,25,0,148,0,20,8,148,0,203,2,7,0,21,8,7,0,22,8,7,0,23,8,2,0,31,0,2,0,99,1,2,0,24,8,2,0,25,8,7,0,26,8,7,0,27,8,
7,0,28,8,0,0,99,0,7,0,29,8,7,0,30,8,7,0,31,8,7,0,32,8,7,0,33,8,7,0,34,8,7,0,35,8,4,0,248,0,4,0,36,8,7,0,37,8,4,0,38,8,7,0,138,6,153,1,5,0,4,0,172,1,0,0,162,6,0,0,39,8,0,0,40,8,0,0,179,2,130,0,46,0,25,0,81,0,98,0,122,1,2,0,31,0,0,0,41,8,0,0,38,4,7,0,171,3,7,0,172,3,7,0,173,3,7,0,174,3,
7,0,42,8,7,0,43,8,7,0,44,8,7,0,71,2,7,0,45,8,7,0,46,8,7,0,47,8,7,0,49,1,7,0,48,8,0,0,90,7,0,0,49,8,2,0,89,7,2,0,50,8,2,0,99,1,88,1,103,7,73,0,254,0,38,0,231,0,7,0,51,8,2,0,52,8,2,0,53,8,2,0,54,8,2,0,55,8,2,0,56,8,0,0,57,8,0,0,58,8,7,0,59,8,7,0,60,8,0,0,61,8,0,0,62,8,0,0,13,8,
0,0,63,8,7,0,64,8,0,0,77,5,151,1,65,8,22,0,66,8,152,1,67,8,153,1,68,8,248,0,56,0,25,0,81,0,98,0,122,1,73,0,254,0,214,0,61,4,130,0,62,4,4,0,38,1,4,0,69,8,4,0,70,8,4,0,71,8,4,0,72,8,117,0,126,4,221,0,73,8,221,0,74,8,221,0,75,8,221,0,76,8,22,0,131,4,4,0,132,4,4,0,133,4,154,1,77,8,4,0,78,8,4,0,79,8,248,0,80,8,
7,0,148,0,7,0,149,0,0,0,64,4,0,0,81,8,3,0,31,0,7,0,82,8,7,0,83,8,7,0,84,8,4,0,85,8,4,0,86,8,0,0,87,8,0,0,88,8,0,0,89,8,0,0,90,8,0,0,137,4,0,0,91,8,2,0,70,4,0,0,92,8,0,0,86,6,0,0,93,8,0,0,94,8,155,1,95,8,156,1,96,8,157,1,97,8,158,1,98,8,6,1,253,5,159,1,99,8,160,1,100,8,161,1,101,8,162,1,102,8,
221,0,103,8,4,0,104,8,0,0,120,0,163,1,54,0,160,1,8,0,11,0,105,8,7,0,106,8,4,0,107,8,0,0,31,0,0,0,108,8,2,0,248,0,2,0,225,6,2,0,109,8,154,1,2,0,4,0,99,1,4,0,29,0,164,1,1,0,7,0,110,8,165,1,1,0,4,0,111,8,166,1,2,0,0,0,112,8,0,0,113,8,167,1,1,0,1,0,173,3,168,1,1,0,12,0,111,8,169,1,2,0,4,0,114,8,7,0,140,1,
6,1,3,0,169,1,115,8,4,0,116,8,4,0,31,0,170,1,2,0,7,0,117,8,4,0,31,0,171,1,4,0,0,0,171,3,0,0,172,3,0,0,173,3,0,0,174,3,172,1,1,0,7,0,227,5,173,1,4,0,4,0,118,8,4,0,207,1,7,0,119,8,4,0,120,8,174,1,3,0,7,0,22,1,4,0,207,1,0,0,2,0,175,1,1,0,0,0,31,0,176,1,1,0,0,0,31,0,158,1,5,0,4,0,121,8,4,0,122,8,
0,0,123,8,0,0,124,8,2,0,31,0,155,1,5,0,4,0,125,8,4,0,71,8,2,0,39,4,0,0,31,0,0,0,179,2,177,1,2,0,7,0,126,8,4,0,31,0,157,1,4,0,7,0,91,0,0,0,31,0,0,0,124,8,0,0,49,0,156,1,2,0,4,0,170,2,4,0,127,8,162,1,7,0,4,0,121,8,4,0,122,8,4,0,128,8,4,0,129,8,2,0,39,4,0,0,130,8,0,0,31,0,159,1,1,0,7,0,106,8,
161,1,4,0,0,0,174,3,0,0,171,3,0,0,172,3,0,0,173,3,178,1,1,0,4,0,111,8,179,1,19,0,179,1,27,0,179,1,28,0,180,1,131,8,2,0,29,0,2,0,31,0,0,0,2,0,7,0,112,3,7,0,113,3,7,0,220,5,7,0,151,0,7,0,132,8,7,0,133,8,7,0,134,8,7,0,135,8,7,0,136,8,7,0,169,2,7,0,35,0,7,0,137,8,7,0,138,8,181,1,19,0,25,0,81,0,98,0,122,1,
22,0,139,8,22,0,140,8,73,0,254,0,130,0,62,4,0,0,31,0,0,0,0,2,2,0,70,4,0,0,64,4,0,0,49,0,0,0,161,1,7,0,148,0,7,0,149,0,7,0,141,8,7,0,142,8,7,0,143,8,0,0,33,0,179,1,144,8,182,1,13,0,182,1,27,0,182,1,28,0,4,0,29,0,4,0,248,0,7,0,145,8,2,0,31,0,2,0,23,1,3,0,146,8,0,0,49,0,4,0,147,8,0,0,32,0,0,0,59,6,
11,0,54,0,183,1,8,0,182,1,60,6,231,0,148,8,52,0,149,8,0,0,150,8,0,0,194,4,0,0,120,0,4,0,151,8,4,0,152,8,184,1,11,0,182,1,60,6,2,0,153,8,2,0,154,8,2,0,155,8,2,0,172,1,2,0,156,8,2,0,157,8,2,0,158,8,0,0,49,0,11,0,159,8,11,0,160,8,185,1,7,0,182,1,60,6,52,0,84,6,0,0,32,0,7,0,46,1,2,0,31,0,0,0,49,0,11,0,75,0,
186,1,7,0,182,1,60,6,52,0,84,6,0,0,32,0,2,0,161,8,2,0,31,0,0,0,2,0,11,0,75,0,187,1,6,0,182,1,60,6,7,0,87,1,7,0,93,0,2,0,31,0,2,0,162,8,4,0,225,4,188,1,7,0,182,1,60,6,52,0,163,8,0,0,24,7,2,0,248,0,2,0,31,0,7,0,164,8,11,0,75,0,189,1,14,0,182,1,60,6,52,0,165,8,52,0,166,8,52,0,167,8,52,0,168,8,7,0,2,4,
7,0,30,6,7,0,93,0,7,0,169,8,4,0,170,8,4,0,171,8,4,0,172,1,4,0,88,6,7,0,172,8,190,1,11,0,182,1,60,6,2,0,142,6,2,0,31,0,7,0,214,7,7,0,173,8,1,0,174,8,0,0,18,0,7,0,172,8,7,0,175,8,52,0,176,8,11,0,75,0,191,1,3,0,182,1,60,6,7,0,177,8,4,0,172,1,192,1,24,0,182,1,60,6,7,0,5,0,4,0,178,8,2,0,172,1,2,0,179,8,
2,0,180,8,2,0,181,8,2,0,182,8,2,0,183,8,2,0,184,8,2,0,185,8,2,0,186,8,2,0,187,8,2,0,188,8,0,0,189,8,0,0,179,2,7,0,190,8,7,0,191,8,7,0,189,6,0,0,192,8,0,0,120,0,215,0,193,8,0,0,194,8,0,0,195,8,244,0,7,0,182,1,60,6,243,0,196,8,247,0,197,8,249,0,198,8,7,0,35,1,4,0,29,0,11,0,75,0,193,1,15,0,182,1,60,6,231,0,148,8,
52,0,149,8,0,0,150,8,0,0,194,4,0,0,120,0,4,0,151,8,4,0,152,8,7,0,46,1,4,0,199,8,0,0,192,8,7,0,200,8,4,0,192,3,2,0,31,0,0,0,111,4,194,1,10,0,182,1,60,6,52,0,201,8,0,0,99,0,4,0,202,8,7,0,203,8,7,0,204,8,7,0,205,8,7,0,206,8,0,0,194,4,4,0,151,8,195,1,11,0,182,1,60,6,7,0,207,8,2,0,208,8,0,0,209,8,0,0,210,8,
7,0,143,6,0,0,192,8,7,0,211,8,2,0,31,0,2,0,248,0,4,0,212,8,196,1,5,0,182,1,60,6,7,0,213,8,0,0,192,8,2,0,31,0,2,0,91,1,197,1,9,0,182,1,60,6,52,0,84,6,7,0,213,8,7,0,27,4,7,0,45,1,0,0,192,8,2,0,31,0,2,0,29,0,11,0,75,0,198,1,24,0,182,1,60,6,231,0,148,8,52,0,149,8,0,0,150,8,0,0,194,4,0,0,120,0,4,0,151,8,
4,0,152,8,52,0,214,8,0,0,192,8,2,0,31,0,0,0,253,1,7,0,254,4,7,0,255,4,7,0,222,1,7,0,71,4,7,0,0,5,7,0,208,1,7,0,232,4,7,0,219,1,7,0,2,5,7,0,222,4,0,0,77,5,11,0,215,8,199,1,7,0,182,1,60,6,2,0,162,1,2,0,126,6,0,0,99,0,52,0,84,6,7,0,127,6,0,0,192,8,200,1,15,0,182,1,60,6,52,0,84,6,0,0,190,3,0,0,31,0,
0,0,118,6,0,0,22,0,7,0,119,6,7,0,120,6,7,0,219,1,129,0,122,6,4,0,216,8,4,0,217,8,7,0,121,6,0,0,32,0,11,0,75,0,201,1,1,0,182,1,60,6,202,1,12,0,182,1,60,6,203,1,218,8,157,0,219,8,160,0,220,8,230,0,221,8,22,0,164,4,204,1,222,8,7,0,223,8,7,0,224,8,4,0,225,8,7,0,226,8,205,1,227,8,206,1,15,0,182,1,60,6,7,0,228,8,7,0,229,8,
7,0,230,8,7,0,231,8,7,0,232,8,7,0,233,8,4,0,234,8,4,0,235,8,4,0,236,8,7,0,237,8,7,0,238,8,0,0,239,8,0,0,47,2,207,1,240,8,208,1,6,0,7,0,241,8,7,0,242,8,248,0,185,5,209,1,240,8,4,0,243,8,4,0,244,8,210,1,2,0,182,1,60,6,208,1,80,0,211,1,10,0,182,1,60,6,52,0,84,6,67,0,92,3,7,0,245,8,0,0,38,0,0,0,205,0,0,0,246,8,
0,0,31,0,0,0,247,8,0,0,47,2,212,1,2,0,4,0,248,8,7,0,140,1,213,1,2,0,4,0,48,1,4,0,249,8,214,1,22,0,182,1,60,6,52,0,84,6,0,0,192,8,2,0,250,8,2,0,31,0,0,0,2,0,212,1,251,8,4,0,252,8,7,0,253,8,4,0,38,1,4,0,254,8,213,1,255,8,212,1,0,9,4,0,1,9,4,0,2,9,4,0,249,8,7,0,3,9,7,0,4,9,7,0,5,9,7,0,6,9,
7,0,7,9,11,0,8,9,215,1,10,0,182,1,60,6,234,0,202,4,248,0,9,9,248,0,10,9,4,0,11,9,4,0,12,9,4,0,13,9,2,0,31,0,0,0,49,0,11,0,75,0,216,1,15,0,182,1,60,6,52,0,198,1,2,0,14,9,2,0,31,0,2,0,142,6,2,0,192,3,7,0,15,9,7,0,16,9,7,0,208,2,7,0,17,9,7,0,18,9,7,0,19,9,0,0,20,9,0,0,21,9,11,0,75,0,217,1,8,0,
182,1,60,6,4,0,22,9,2,0,31,0,2,0,23,9,7,0,24,9,0,0,25,9,0,0,120,0,11,0,188,0,218,1,12,0,182,1,60,6,0,0,26,9,0,0,27,9,0,0,28,9,0,0,29,9,0,0,30,9,0,0,172,1,0,0,49,0,2,0,157,8,2,0,156,8,2,0,158,8,0,0,253,1,219,1,3,0,182,1,60,6,220,1,31,9,11,0,75,0,221,1,3,0,182,1,60,6,4,0,29,0,4,0,179,2,222,1,12,0,
182,1,60,6,52,0,9,4,52,0,32,9,0,0,33,9,7,0,34,9,2,0,10,4,0,0,35,9,0,0,14,4,7,0,13,4,0,0,11,4,0,0,36,9,0,0,49,0,223,1,10,0,182,1,60,6,52,0,37,9,0,0,33,9,7,0,64,6,7,0,38,9,0,0,248,0,0,0,142,6,0,0,39,9,0,0,31,0,11,0,75,0,224,1,1,0,182,1,60,6,225,1,20,0,182,1,60,6,0,0,192,8,0,0,40,9,0,0,41,9,
7,0,48,1,7,0,221,3,7,0,42,9,7,0,43,9,0,0,248,0,0,0,44,9,0,0,45,9,0,0,179,2,7,0,46,9,7,0,47,9,7,0,48,9,4,0,31,0,2,0,49,9,2,0,50,9,7,0,51,9,7,0,52,9,226,1,12,0,182,1,60,6,52,0,53,9,4,0,54,9,4,0,55,9,4,0,208,8,7,0,56,9,7,0,143,6,7,0,169,8,2,0,31,0,0,0,142,6,0,0,59,1,11,0,75,0,227,1,34,0,
182,1,60,6,228,1,57,9,229,1,58,9,4,0,59,9,4,0,60,9,4,0,61,9,7,0,62,9,7,0,232,4,7,0,63,9,7,0,51,1,7,0,64,9,7,0,65,9,7,0,66,9,7,0,67,9,7,0,68,9,7,0,35,1,4,0,69,9,7,0,70,9,7,0,71,9,4,0,72,9,4,0,73,9,0,0,74,9,0,0,75,9,0,0,76,9,0,0,77,9,0,0,78,9,0,0,31,0,0,0,252,2,2,0,79,9,2,0,80,9,
4,0,225,4,7,0,45,1,7,0,81,9,0,0,2,0,230,1,20,0,182,1,60,6,231,0,148,8,52,0,149,8,0,0,150,8,0,0,194,4,0,0,120,0,4,0,151,8,4,0,152,8,52,0,82,9,52,0,83,9,0,0,84,9,0,0,85,9,129,0,122,6,0,0,192,8,7,0,46,1,7,0,86,9,0,0,31,0,0,0,118,6,0,0,111,4,11,0,98,1,231,1,17,0,182,1,60,6,0,0,192,8,2,0,87,9,2,0,118,6,
7,0,88,9,129,0,89,9,7,0,90,9,7,0,91,9,7,0,92,9,0,0,93,9,4,0,94,9,231,0,95,9,52,0,96,9,0,0,97,9,4,0,98,9,0,0,99,9,11,0,75,0,232,1,19,0,182,1,60,6,0,0,100,9,0,0,101,9,7,0,102,9,7,0,103,9,0,0,214,3,0,0,104,9,0,0,242,0,7,0,92,9,0,0,93,9,4,0,94,9,231,0,95,9,52,0,96,9,0,0,97,9,4,0,98,9,0,0,99,9,
0,0,120,0,0,0,31,0,0,0,191,2,233,1,19,0,182,1,60,6,0,0,192,8,129,0,89,9,4,0,105,9,4,0,106,9,52,0,107,9,7,0,92,9,0,0,93,9,4,0,94,9,231,0,95,9,52,0,96,9,0,0,97,9,4,0,98,9,0,0,99,9,0,0,120,0,7,0,108,9,7,0,109,9,2,0,118,6,0,0,40,0,232,0,5,0,182,1,60,6,228,0,161,4,233,0,110,9,4,0,29,0,0,0,2,0,234,1,10,0,
182,1,60,6,7,0,164,8,7,0,92,1,7,0,111,9,0,0,51,1,0,0,31,0,0,0,248,0,0,0,179,2,7,0,112,9,7,0,113,9,235,1,5,0,182,1,60,6,7,0,114,9,0,0,31,0,0,0,115,9,0,0,49,0,236,1,5,0,182,1,60,6,4,0,31,0,4,0,116,9,4,0,117,9,4,0,118,9,237,1,7,0,182,1,60,6,7,0,119,9,7,0,120,9,0,0,120,0,0,0,192,8,2,0,31,0,2,0,91,1,
238,1,9,0,7,0,121,9,4,0,122,9,7,0,119,9,7,0,92,1,2,0,91,1,2,0,31,0,0,0,123,9,0,0,124,9,0,0,22,0,239,1,12,0,182,1,60,6,7,0,125,9,4,0,126,9,7,0,119,9,7,0,92,1,2,0,91,1,2,0,31,0,0,0,123,9,0,0,124,9,0,0,22,0,0,0,192,8,238,1,127,9,240,1,15,0,182,1,60,6,0,0,128,9,0,0,129,9,2,0,31,0,7,0,130,9,7,0,207,2,
7,0,43,6,7,0,208,2,52,0,131,9,0,0,132,9,52,0,133,9,0,0,134,9,0,0,33,9,0,0,194,4,0,0,2,0,241,1,19,0,182,1,60,6,0,0,31,0,0,0,29,0,0,0,98,6,0,0,135,9,0,0,183,2,0,0,184,2,0,0,136,9,0,0,19,8,7,0,64,6,0,0,137,9,0,0,192,8,0,0,47,2,7,0,229,0,7,0,79,6,7,0,138,9,7,0,218,3,7,0,139,9,0,0,178,2,242,1,8,0,
182,1,60,6,0,0,140,9,4,0,141,9,4,0,91,1,7,0,142,9,11,0,143,9,2,0,31,0,0,0,22,0,243,1,9,0,182,1,60,6,0,0,192,8,7,0,48,1,7,0,221,3,7,0,42,9,7,0,144,9,2,0,31,0,2,0,49,9,0,0,2,0,244,1,6,0,182,1,60,6,7,0,169,8,0,0,192,8,0,0,248,0,0,0,31,0,0,0,49,0,245,1,18,0,182,1,60,6,52,0,145,9,4,0,146,9,4,0,147,9,
4,0,148,9,4,0,149,9,4,0,150,9,7,0,151,9,7,0,152,9,7,0,153,9,0,0,120,0,4,0,154,9,4,0,155,9,4,0,214,3,7,0,26,8,0,0,192,8,4,0,172,1,11,0,188,0,246,1,12,0,182,1,60,6,0,0,192,8,52,0,9,4,2,0,248,0,2,0,31,0,2,0,214,3,0,0,49,0,7,0,26,8,7,0,156,9,7,0,2,4,0,0,33,0,11,0,75,0,247,1,8,0,182,1,60,6,143,0,21,4,
0,0,22,4,0,0,157,9,0,0,18,0,7,0,172,5,207,0,23,4,0,0,24,4,248,1,6,0,4,0,158,9,4,0,188,5,4,0,248,0,7,0,159,9,7,0,160,9,7,0,1,1,249,1,3,0,248,1,161,9,4,0,162,9,4,0,163,9,250,1,15,0,182,1,60,6,31,0,61,0,52,0,9,4,249,1,164,9,11,0,75,0,7,0,219,1,4,0,165,9,4,0,188,5,4,0,166,9,4,0,167,9,4,0,172,1,7,0,135,7,
7,0,46,1,0,0,192,8,4,0,252,2,251,1,6,0,182,1,60,6,0,0,192,8,0,0,248,0,0,0,31,0,2,0,140,1,7,0,143,8,252,1,1,0,23,0,73,0,253,1,5,0,0,0,168,9,0,0,169,9,25,0,55,1,4,0,21,0,0,0,2,0,254,1,2,0,0,0,4,0,36,0,170,9,255,1,4,0,4,0,171,9,4,0,172,9,254,1,173,9,254,1,174,9,0,2,14,0,4,0,81,0,4,0,31,0,1,0,175,9,
12,0,176,9,0,0,22,0,0,0,177,9,4,0,229,0,4,0,230,0,4,0,178,9,4,0,179,9,253,1,180,9,255,1,181,9,11,0,188,0,9,0,182,9,1,2,2,0,4,0,81,0,4,0,31,0,2,2,13,0,182,1,60,6,88,1,183,9,252,1,184,9,0,0,185,9,12,0,31,0,12,0,176,9,0,0,49,0,4,0,186,9,0,2,187,9,0,0,99,0,4,0,188,9,1,2,189,9,3,2,54,0,4,2,9,0,182,1,60,6,
52,0,84,6,4,0,190,9,7,0,112,9,4,0,191,9,7,0,192,9,7,0,76,2,0,0,99,0,11,0,98,1,5,2,7,0,182,1,60,6,231,0,148,8,52,0,193,9,4,0,194,9,7,0,46,1,7,0,195,9,7,0,196,9,6,2,10,0,182,1,60,6,52,0,84,6,7,0,164,8,7,0,113,9,4,0,31,0,4,0,190,9,7,0,112,9,4,0,191,9,0,0,197,9,11,0,75,0,7,2,9,0,4,0,31,0,0,0,120,0,
0,0,198,9,130,0,37,2,4,0,70,6,4,0,199,9,0,0,200,9,129,0,201,9,11,0,188,0,8,2,8,0,182,1,60,6,7,2,1,1,4,0,31,0,0,0,79,1,0,0,133,1,7,0,202,9,7,0,203,9,11,0,188,0,9,2,6,0,182,1,60,6,7,2,1,1,4,0,29,0,4,0,207,1,0,0,204,9,11,0,75,0,10,2,6,0,182,1,60,6,7,2,1,1,0,0,79,1,0,0,133,1,7,0,81,6,11,0,188,0,
11,2,11,0,182,1,60,6,7,2,1,1,2,0,31,0,0,0,79,1,0,0,205,9,7,0,64,6,7,0,27,4,7,0,96,0,52,0,84,6,135,0,187,7,11,0,103,0,12,2,7,0,182,1,60,6,7,2,1,1,4,0,31,0,7,0,64,6,4,0,14,0,0,0,2,0,11,0,75,0,13,2,15,0,182,1,60,6,7,2,1,1,4,0,31,0,4,0,4,8,7,0,148,0,7,0,123,6,7,0,30,6,7,0,206,9,7,0,207,9,
7,0,208,9,4,0,225,4,4,0,124,6,4,0,125,6,0,0,120,0,11,0,188,0,14,2,14,0,182,1,60,6,7,2,1,1,4,0,31,0,7,0,64,6,7,0,65,6,7,0,66,6,7,0,67,6,7,0,76,5,7,0,68,6,2,0,69,6,0,0,49,0,4,0,14,0,4,0,225,4,11,0,75,0,15,2,5,0,182,1,60,6,7,2,1,1,52,0,84,6,4,0,31,0,0,0,2,0,16,2,7,0,182,1,60,6,7,2,1,1,
4,0,31,0,7,0,72,6,7,0,243,5,0,0,2,0,11,0,75,0,17,2,5,0,182,1,60,6,7,2,1,1,52,0,84,6,7,0,46,1,0,0,2,0,18,2,7,0,0,0,32,0,4,0,115,6,4,0,116,6,7,0,27,4,7,0,18,6,4,0,39,4,4,0,31,0,19,2,7,0,182,1,60,6,7,2,1,1,18,2,209,9,4,0,210,9,4,0,80,6,4,0,117,6,0,0,2,0,20,2,11,0,182,1,60,6,7,2,1,1,
4,0,31,0,4,0,128,6,7,0,204,1,7,0,48,1,7,0,129,6,7,0,130,6,7,0,131,6,4,0,95,3,11,0,103,0,21,2,17,0,182,1,60,6,7,2,1,1,4,0,31,0,7,0,105,6,7,0,106,6,7,0,107,6,7,0,108,6,7,0,109,6,7,0,110,6,4,0,225,4,4,0,14,0,4,0,248,0,0,0,2,0,7,0,111,6,7,0,112,6,7,0,113,6,11,0,75,0,22,2,9,0,182,1,60,6,7,2,1,1,
4,0,31,0,7,0,139,6,2,0,142,6,2,0,192,3,7,0,143,6,0,0,101,6,11,0,103,0,23,2,13,0,182,1,60,6,7,2,1,1,52,0,84,6,4,0,88,6,4,0,31,0,7,0,2,4,7,0,41,5,7,0,89,6,7,0,90,6,7,0,91,6,0,0,2,0,4,0,225,4,4,0,92,6,24,2,8,0,182,1,60,6,7,2,1,1,4,0,31,0,0,0,101,6,7,0,139,6,7,0,140,6,7,0,141,6,52,0,84,6,
25,2,12,0,182,1,60,6,7,2,1,1,52,0,84,6,0,0,190,3,0,0,2,0,4,0,31,0,0,0,118,6,0,0,133,1,7,0,119,6,7,0,120,6,7,0,219,1,7,0,121,6,26,2,44,0,182,1,60,6,3,0,209,5,0,0,145,6,0,0,146,6,2,0,147,6,2,0,148,6,52,0,149,6,52,0,150,6,52,0,151,6,67,0,152,6,130,0,153,6,0,0,154,6,0,0,155,6,0,0,63,6,7,0,156,6,7,0,157,6,
7,0,158,6,7,0,159,6,7,0,160,6,7,0,18,6,2,0,243,5,0,0,211,9,0,0,212,9,0,0,163,6,0,0,164,6,0,0,165,6,0,0,255,1,7,0,166,6,7,0,167,6,7,0,168,6,7,0,169,6,4,0,170,6,4,0,172,1,7,0,171,6,0,0,172,6,0,0,173,6,2,0,174,6,0,0,175,6,0,0,176,6,0,0,111,4,43,1,213,9,43,1,8,3,44,1,177,6,27,2,54,0,28,2,5,0,182,1,60,6,
7,2,1,1,52,0,84,6,2,0,162,1,0,0,22,0,29,2,5,0,0,0,32,0,4,0,214,9,4,0,215,9,4,0,216,9,4,0,217,9,30,2,11,0,182,1,60,6,7,2,1,1,4,0,31,0,4,0,48,1,7,0,79,6,4,0,248,0,4,0,24,1,4,0,25,1,29,2,209,9,4,0,210,9,4,0,80,6,31,2,8,0,182,1,60,6,7,2,1,1,4,0,248,0,4,0,39,4,7,0,243,5,7,0,46,1,4,0,188,6,
4,0,189,6,32,2,8,0,182,1,60,6,7,2,1,1,52,0,84,6,4,0,31,0,4,0,243,5,7,0,85,6,4,0,86,6,130,0,87,6,33,2,15,0,182,1,60,6,7,2,1,1,52,0,9,4,52,0,178,6,7,0,179,6,2,0,180,6,0,0,181,6,0,0,182,6,7,0,183,6,0,0,184,6,0,0,185,6,0,0,49,0,7,0,186,6,4,0,187,6,46,1,104,6,34,2,20,0,182,1,60,6,7,2,1,1,7,0,94,0,
7,0,95,0,7,0,93,6,7,0,93,0,2,0,31,0,2,0,248,0,2,0,94,6,2,0,95,6,7,0,96,6,7,0,97,6,2,0,98,6,0,0,22,0,52,0,84,6,7,0,99,6,7,0,100,6,0,0,101,6,7,0,102,6,7,0,103,6,35,2,9,0,182,1,60,6,7,2,1,1,2,0,248,0,0,0,2,0,2,0,14,0,7,0,64,6,7,0,93,0,7,0,154,2,7,0,204,1,36,2,12,0,182,1,60,6,7,2,1,1,
7,0,133,6,7,0,250,5,7,0,134,6,7,0,135,6,7,0,136,6,4,0,70,6,2,0,137,6,2,0,248,0,7,0,138,6,0,0,2,0,151,0,3,0,4,0,221,6,2,0,218,9,2,0,219,9,37,2,5,0,0,0,220,9,2,0,221,9,2,0,157,8,2,0,222,9,2,0,223,9,38,2,4,0,11,0,27,0,11,0,28,0,151,0,224,9,246,0,225,9,39,2,2,0,22,0,226,9,10,0,248,7,150,0,21,0,25,0,81,0,
98,0,122,1,0,0,82,0,4,0,209,2,4,0,179,2,4,0,227,9,7,0,0,7,7,0,1,7,64,1,228,6,40,2,8,3,14,1,228,9,11,0,75,0,41,2,229,9,11,0,230,9,37,2,231,9,4,0,31,0,4,0,35,0,4,0,94,0,4,0,13,1,173,0,2,7,39,2,80,0,42,2,15,0,2,0,145,3,2,0,232,9,4,0,233,9,4,0,234,9,4,0,235,9,43,2,236,9,134,0,237,9,134,0,238,9,7,0,239,9,
2,0,240,9,2,0,241,9,4,0,242,9,44,2,243,9,43,2,244,9,7,0,245,9,45,2,10,0,45,2,27,0,45,2,28,0,2,0,29,0,2,0,31,0,0,0,246,9,7,0,247,9,7,0,248,9,2,0,210,0,2,0,249,9,52,0,198,1,46,2,22,0,46,2,27,0,46,2,28,0,2,0,31,0,2,0,248,0,2,0,250,9,2,0,251,9,73,0,254,0,62,0,83,1,52,0,84,6,7,0,87,1,7,0,88,1,7,0,89,1,
7,0,90,1,7,0,252,9,7,0,253,9,7,0,91,1,7,0,92,1,7,0,26,1,7,0,27,1,0,0,254,9,0,0,255,9,22,0,72,1,47,2,2,0,0,0,0,10,0,0,47,2,48,2,13,0,47,2,1,10,0,0,4,0,0,0,0,0,0,0,2,10,4,0,31,0,2,0,3,10,2,0,4,10,0,0,5,10,0,0,3,0,11,0,6,10,23,0,73,0,12,0,7,10,0,0,47,2,49,2,8,0,47,2,1,10,0,0,4,0,
0,0,0,0,4,0,31,0,0,0,2,0,47,2,8,10,4,0,9,10,4,0,10,10,50,2,4,0,49,2,11,10,4,0,12,10,4,0,13,10,51,2,54,0,52,2,11,0,7,0,37,4,7,0,10,0,7,0,11,0,11,0,22,1,2,0,14,10,2,0,15,10,2,0,16,10,2,0,17,10,2,0,18,10,2,0,19,10,0,0,2,0,53,2,26,0,53,2,27,0,53,2,28,0,23,0,126,0,0,0,20,10,0,0,32,0,11,0,21,10,
2,0,29,0,2,0,31,0,2,0,22,10,2,0,23,10,54,2,24,10,0,0,105,1,11,0,20,0,2,0,25,10,0,0,26,10,0,0,3,10,0,0,2,0,0,0,240,6,0,0,27,10,0,0,28,10,0,0,5,10,4,0,29,10,4,0,30,10,55,2,31,10,52,2,32,10,56,2,54,0,57,2,3,0,4,0,10,10,0,0,31,0,0,0,18,0,58,2,33,0,58,2,27,0,58,2,28,0,22,0,33,10,22,0,34,10,0,0,32,0,
4,0,10,10,4,0,31,0,0,0,105,1,59,2,24,10,2,0,29,0,2,0,35,10,2,0,36,10,2,0,37,10,7,0,38,10,7,0,39,10,12,0,40,10,0,0,47,2,25,0,55,1,11,0,21,10,23,0,126,0,58,2,136,0,7,0,41,10,7,0,71,4,7,0,222,1,7,0,42,10,7,0,43,10,7,0,44,10,7,0,45,10,0,0,240,6,7,0,96,0,4,0,46,10,57,2,47,10,60,2,54,0,61,2,1,0,4,0,5,0,
55,2,8,0,55,2,27,0,55,2,28,0,58,2,48,10,58,2,49,10,53,2,50,10,53,2,51,10,4,0,31,0,4,0,52,10,62,2,2,0,4,0,53,10,4,0,54,10,63,2,3,0,4,0,81,0,0,0,2,0,62,2,55,10,88,1,28,0,25,0,81,0,98,0,122,1,25,0,100,3,64,2,24,10,0,0,105,1,0,0,0,0,14,1,228,9,7,0,56,10,22,0,57,10,22,0,58,10,4,0,29,0,4,0,59,10,4,0,31,0,
4,0,60,10,4,0,61,10,4,0,19,0,4,0,106,3,4,0,62,10,84,0,63,10,22,0,33,10,22,0,34,10,50,2,64,10,61,2,65,10,4,0,66,10,63,2,67,10,65,2,68,10,38,0,231,0,66,2,54,0,67,2,4,0,4,0,30,0,4,0,5,0,4,0,10,0,4,0,11,0,68,2,4,0,4,0,30,0,7,0,5,0,7,0,10,0,7,0,11,0,69,2,1,0,0,0,5,0,70,2,5,0,4,0,30,0,7,0,69,10,
7,0,10,0,7,0,11,0,4,0,70,10,71,2,1,0,7,0,71,10,72,2,1,0,7,0,69,10,73,2,3,0,4,0,30,0,0,0,2,0,0,0,72,10,74,2,1,0,52,0,73,10,75,2,1,0,148,0,73,10,76,2,1,0,67,0,73,10,77,2,1,0,231,0,73,10,78,2,1,0,130,0,73,10,79,2,3,0,4,0,5,0,4,0,55,7,80,2,17,0,65,2,1,0,4,0,31,0,81,2,2,0,2,0,31,0,2,0,74,10,
82,2,1,0,0,0,75,10,83,2,6,0,4,0,8,6,4,0,24,1,4,0,76,10,0,0,77,10,0,0,78,10,0,0,49,0,84,2,6,0,7,0,79,10,7,0,80,10,7,0,166,3,7,0,81,10,7,0,82,10,0,0,2,0,85,2,6,0,84,2,83,10,84,2,84,10,84,2,85,10,84,2,86,10,7,0,87,10,7,0,88,10,86,2,5,0,7,0,143,6,4,0,89,10,7,0,90,10,7,0,91,10,7,0,92,10,87,2,6,0,
7,0,112,3,7,0,113,3,7,0,208,2,7,0,222,1,7,0,71,4,0,0,2,0,88,2,6,0,7,0,112,3,7,0,113,3,7,0,208,2,7,0,222,1,7,0,71,4,0,0,2,0,89,2,2,0,4,0,12,6,0,0,93,10,90,2,16,0,2,0,94,10,2,0,95,10,2,0,67,7,2,0,96,10,2,0,97,10,2,0,22,7,2,0,98,10,2,0,99,10,7,0,213,8,7,0,100,10,7,0,101,10,2,0,102,10,0,0,103,10,
0,0,166,3,4,0,104,10,4,0,105,10,91,2,8,0,7,0,106,10,7,0,107,10,7,0,204,1,7,0,143,6,7,0,108,10,7,0,109,10,2,0,208,8,0,0,49,0,92,2,4,0,7,0,110,10,7,0,111,10,2,0,208,8,0,0,49,0,93,2,7,0,2,0,45,1,2,0,112,10,4,0,113,10,7,0,114,10,7,0,115,10,0,0,116,10,0,0,18,0,94,2,3,0,7,0,164,8,7,0,117,10,7,0,118,10,95,2,3,0,
7,0,119,10,7,0,120,10,7,0,25,0,96,2,4,0,0,0,82,0,97,2,121,10,4,0,24,1,4,0,25,1,98,2,7,0,0,0,122,10,97,2,166,4,4,0,24,1,4,0,25,1,4,0,123,10,0,0,124,10,0,0,18,0,99,2,8,0,2,0,125,10,2,0,126,10,0,0,124,10,0,0,133,1,0,0,127,10,97,2,166,4,0,0,128,10,0,0,253,1,100,2,9,0,7,0,129,10,7,0,130,10,7,0,131,10,7,0,85,4,
7,0,132,10,7,0,133,10,7,0,134,10,2,0,135,10,2,0,136,10,101,2,8,0,2,0,137,10,2,0,138,10,2,0,139,10,2,0,140,10,7,0,141,10,7,0,142,10,7,0,143,10,7,0,144,10,102,2,2,0,7,0,112,3,7,0,113,3,103,2,1,0,0,0,32,0,104,2,2,0,1,0,248,0,1,0,145,10,105,2,12,0,0,0,146,10,0,0,95,3,0,0,147,10,0,0,148,10,2,0,67,7,2,0,149,10,7,0,190,6,
7,0,150,10,7,0,151,10,7,0,92,1,7,0,208,2,0,0,120,0,106,2,2,0,11,0,152,10,11,0,153,10,107,2,14,0,0,0,29,0,0,0,157,8,0,0,208,8,0,0,143,6,0,0,95,3,0,0,45,1,0,0,154,10,0,0,155,10,7,0,156,10,7,0,157,10,7,0,164,8,7,0,158,10,7,0,159,10,0,0,120,0,108,2,8,0,7,0,160,10,7,0,48,1,7,0,166,3,7,0,110,8,7,0,161,10,7,0,174,3,
7,0,162,10,4,0,29,0,109,2,5,0,2,0,163,10,2,0,164,10,2,0,165,10,0,0,49,0,4,0,166,10,110,2,12,0,7,0,167,10,7,0,2,4,7,0,168,10,7,0,169,10,0,0,2,0,7,0,170,10,7,0,171,10,7,0,172,10,7,0,173,10,7,0,174,10,7,0,175,10,7,0,176,10,111,2,6,0,2,0,177,10,2,0,178,10,7,0,179,10,7,0,180,10,7,0,181,10,7,0,182,10,112,2,2,0,0,0,183,10,
0,0,184,10,113,2,1,0,0,0,219,1,114,2,2,0,4,0,185,10,4,0,186,10,115,2,1,0,0,0,248,0,116,2,2,0,117,2,187,10,118,2,188,10,119,2,15,0,116,2,7,0,4,0,189,10,7,0,190,10,7,0,191,10,7,0,192,10,7,0,193,10,7,0,194,10,7,0,195,10,7,0,196,10,7,0,197,10,7,0,198,10,7,0,199,10,7,0,200,10,0,0,201,10,0,0,47,2,120,2,8,0,116,2,7,0,149,0,204,2,
4,0,202,10,4,0,203,10,7,0,204,10,4,0,205,10,4,0,206,10,0,0,2,0,121,2,1,0,116,2,7,0,122,2,5,0,116,2,7,0,4,0,207,10,4,0,208,10,7,0,48,1,7,0,209,10,123,2,6,0,116,2,7,0,149,0,204,2,4,0,202,10,4,0,203,10,4,0,205,10,0,0,2,0,124,2,3,0,116,2,7,0,0,0,29,0,0,0,47,2,125,2,3,0,116,2,7,0,4,0,36,8,0,0,2,0,126,2,5,0,
116,2,7,0,4,0,70,10,1,0,29,0,1,0,210,10,0,0,49,0,127,2,7,0,116,2,7,0,4,0,70,10,4,0,211,10,4,0,204,1,4,0,210,10,4,0,212,10,0,0,2,0,128,2,3,0,116,2,7,0,4,0,213,10,4,0,70,10,129,2,5,0,116,2,7,0,4,0,211,4,4,0,214,10,4,0,215,10,4,0,216,10,130,2,3,0,116,2,7,0,4,0,51,1,0,0,2,0,131,2,3,0,0,0,217,10,4,0,29,0,
0,0,2,0,132,2,4,0,4,0,29,0,4,0,218,10,4,0,219,10,0,0,2,0,133,2,13,0,116,2,7,0,2,0,220,10,0,0,49,0,4,0,221,10,7,0,27,4,4,0,59,9,2,0,192,3,2,0,205,10,2,0,222,10,2,0,223,10,134,2,224,10,4,0,225,10,0,0,226,10,135,2,2,0,0,0,227,10,0,0,18,0,136,2,3,0,2,0,228,10,2,0,229,10,0,0,2,0,137,2,1,0,0,0,32,0,138,2,2,0,
0,0,230,10,7,0,231,10,139,2,12,0,7,0,232,10,7,0,233,10,7,0,234,10,4,0,235,10,7,0,236,10,7,0,237,10,7,0,238,10,4,0,239,10,4,0,240,10,4,0,241,10,4,0,242,10,4,0,243,10,140,2,2,0,0,0,230,10,0,0,244,10,141,2,3,0,0,0,245,10,0,0,22,7,2,0,205,10,142,2,1,0,2,0,205,10,143,2,6,0,0,0,230,10,0,0,246,10,0,0,31,0,0,0,247,10,0,0,49,0,
7,0,248,10,144,2,5,0,4,0,248,0,4,0,31,0,0,0,178,2,0,0,249,10,0,0,250,10,145,2,3,0,4,0,251,10,4,0,142,6,0,0,252,10,146,2,2,0,4,0,192,3,0,0,252,10,147,2,1,0,0,0,252,10,148,2,1,0,0,0,198,9,149,2,2,0,4,0,248,0,0,0,178,2,150,2,1,0,0,0,32,0,151,2,2,0,7,0,253,10,7,0,254,10,152,2,5,0,152,2,27,0,152,2,28,0,7,0,255,10,
0,0,32,0,0,0,2,0,153,2,3,0,152,2,27,0,152,2,28,0,0,0,32,0,154,2,3,0,22,0,181,2,7,0,0,11,7,0,1,11,155,2,7,0,149,0,204,2,22,0,2,11,0,0,198,9,0,0,3,11,4,0,4,11,0,0,2,0,154,2,80,0,156,2,4,0,0,0,5,11,0,0,6,11,0,0,157,8,0,0,255,1,157,2,4,0,1,0,191,1,1,0,7,11,1,0,88,3,0,0,59,1,158,2,1,0,1,0,191,1,
159,2,2,0,1,0,191,1,1,0,192,1,160,2,1,0,1,0,8,11,161,2,1,0,4,0,9,11,162,2,1,0,7,0,10,11,163,2,1,0,7,0,11,11,164,2,1,0,7,0,227,5,165,2,1,0,0,0,12,11,166,2,1,0,1,0,248,0,167,2,1,0,1,0,13,11,168,2,2,0,1,0,190,9,1,0,14,11,169,2,1,0,1,0,13,11,170,2,1,0,1,0,15,11,171,2,1,0,1,0,190,9,172,2,1,0,1,0,190,9,
173,2,2,0,1,0,156,8,1,0,158,8,174,2,1,0,1,0,16,11,175,2,1,0,1,0,16,11,176,2,1,0,1,0,16,11,177,2,1,0,1,0,248,0,178,2,2,0,1,0,248,0,1,0,17,11,179,2,1,0,1,0,18,11,180,2,4,0,0,0,4,0,0,0,0,0,4,0,10,10,0,0,2,0,181,2,5,0,180,2,19,11,4,0,9,10,4,0,12,10,4,0,20,11,0,0,2,0,182,2,3,0,181,2,21,11,1,0,191,1,
0,0,47,2,183,2,1,0,1,0,22,11,184,2,1,0,1,0,248,0,185,2,2,0,1,0,23,11,1,0,248,0,186,2,2,0,1,0,23,11,1,0,248,0,187,2,1,0,1,0,248,0,188,2,1,0,1,0,248,0,189,2,1,0,1,0,248,0,190,2,1,0,1,0,248,0,191,2,1,0,1,0,248,0,192,2,2,0,1,0,248,0,1,0,24,11,193,2,1,0,1,0,248,0,194,2,1,0,1,0,248,0,195,2,1,0,1,0,248,0,
196,2,4,0,1,0,248,0,12,0,25,11,12,0,191,1,0,0,255,1,197,2,4,0,12,0,191,1,12,0,192,1,1,0,248,0,0,0,255,1,198,2,4,0,12,0,191,1,12,0,192,1,12,0,88,3,0,0,255,1,199,2,2,0,1,0,26,11,12,0,191,1,200,2,1,0,1,0,248,0,201,2,1,0,1,0,248,0,202,2,4,0,12,0,191,1,0,0,18,0,4,0,10,10,0,0,4,0,203,2,7,0,12,0,191,1,12,0,192,1,
0,0,49,0,4,0,20,11,202,2,27,11,4,0,28,11,4,0,12,10,204,2,2,0,12,0,191,1,12,0,192,1,205,2,1,0,12,0,191,1,206,2,4,0,1,0,78,4,1,0,29,11,1,0,80,4,1,0,30,11,207,2,2,0,12,0,192,1,12,0,248,0,208,2,1,0,12,0,192,1,209,2,1,0,12,0,248,0,210,2,1,0,12,0,192,1,211,2,2,0,12,0,205,10,12,0,206,10,212,2,2,0,12,0,191,1,12,0,192,1,
213,2,1,0,1,0,31,11,214,2,4,0,0,0,4,0,2,0,32,11,2,0,3,10,4,0,10,10,215,2,1,0,4,0,33,11,216,2,5,0,214,2,34,11,4,0,9,10,4,0,12,10,4,0,20,11,4,0,179,2,217,2,4,0,0,0,4,0,2,0,32,11,0,0,49,0,4,0,10,10,218,2,1,0,4,0,33,11,219,2,5,0,217,2,34,11,4,0,9,10,4,0,12,10,4,0,20,11,4,0,35,11,220,2,1,0,4,0,33,11,
221,2,4,0,0,0,4,0,2,0,32,11,0,0,49,0,4,0,10,10,222,2,4,0,0,0,4,0,2,0,32,11,0,0,49,0,4,0,10,10,223,2,5,0,0,0,4,0,2,0,32,11,1,0,192,1,0,0,255,1,4,0,10,10,224,2,5,0,221,2,34,11,4,0,9,10,4,0,12,10,4,0,20,11,0,0,2,0,225,2,5,0,222,2,34,11,4,0,9,10,4,0,12,10,4,0,20,11,0,0,2,0,226,2,5,0,223,2,34,11,
4,0,9,10,4,0,12,10,4,0,20,11,0,0,2,0,227,2,6,0,224,2,36,11,225,2,37,11,226,2,38,11,4,0,35,11,1,0,192,1,0,0,18,0,228,2,1,0,4,0,33,11,229,2,4,0,0,0,4,0,2,0,32,11,0,0,49,0,4,0,10,10,230,2,4,0,0,0,4,0,2,0,32,11,0,0,49,0,4,0,10,10,231,2,5,0,229,2,34,11,4,0,9,10,4,0,12,10,4,0,20,11,0,0,2,0,232,2,5,0,
230,2,34,11,4,0,9,10,4,0,12,10,4,0,20,11,0,0,2,0,233,2,2,0,231,2,36,11,232,2,39,11,234,2,4,0,0,0,4,0,2,0,32,11,0,0,49,0,4,0,10,10,235,2,4,0,0,0,4,0,2,0,32,11,0,0,49,0,4,0,10,10,236,2,5,0,234,2,34,11,4,0,9,10,4,0,12,10,4,0,20,11,0,0,2,0,237,2,5,0,235,2,34,11,4,0,9,10,4,0,12,10,4,0,20,11,0,0,2,0,
238,2,2,0,236,2,36,11,237,2,39,11,239,2,1,0,4,0,10,10,240,2,5,0,239,2,34,11,4,0,9,10,4,0,191,1,4,0,20,11,0,0,2,0,241,2,1,0,1,0,248,0,242,2,4,0,12,0,38,0,12,0,191,1,12,0,248,0,0,0,255,1,243,2,1,0,12,0,248,0,244,2,6,0,12,0,191,1,12,0,40,11,12,0,41,11,12,0,42,11,12,0,43,11,0,0,18,0,245,2,2,0,4,0,44,11,4,0,45,11,
246,2,1,0,4,0,44,11,247,2,1,0,4,0,31,0,248,2,6,0,0,0,4,0,2,0,32,11,2,0,3,10,4,0,10,10,4,0,31,0,0,0,2,0,249,2,5,0,248,2,34,11,4,0,9,10,4,0,20,11,4,0,12,10,0,0,2,0,250,2,4,0,0,0,4,0,4,0,10,10,2,0,32,11,0,0,49,0,251,2,5,0,250,2,34,11,4,0,9,10,4,0,20,11,4,0,12,10,0,0,2,0,252,2,4,0,0,0,4,0,
4,0,10,10,2,0,32,11,0,0,49,0,253,2,5,0,252,2,34,11,4,0,9,10,4,0,20,11,4,0,12,10,0,0,2,0,254,2,4,0,0,0,4,0,4,0,10,10,2,0,32,11,0,0,49,0,255,2,5,0,254,2,34,11,4,0,9,10,4,0,20,11,4,0,12,10,0,0,2,0,0,3,1,0,7,0,46,11,220,1,52,0,219,1,14,5,4,0,47,11,0,0,120,0,2,0,29,0,2,0,48,11,2,0,49,11,2,0,50,11,
7,0,51,11,2,0,52,11,2,0,53,11,7,0,54,11,2,0,55,11,2,0,56,11,7,0,57,11,7,0,58,11,7,0,59,11,4,0,60,11,4,0,61,11,4,0,62,11,0,0,99,0,7,0,63,11,4,0,64,11,7,0,65,11,7,0,66,11,7,0,67,11,0,0,68,11,7,0,69,11,7,0,70,11,73,0,254,0,2,0,71,11,0,0,72,11,0,0,73,11,7,0,74,11,4,0,75,11,7,0,76,11,7,0,77,11,4,0,78,11,
4,0,31,0,7,0,79,11,7,0,80,11,7,0,81,11,0,3,82,11,4,0,38,1,7,0,83,11,7,0,84,11,7,0,85,11,7,0,86,11,7,0,87,11,7,0,88,11,7,0,89,11,4,0,90,11,7,0,91,11,1,3,50,0,4,0,31,0,2,0,92,11,2,0,93,11,2,0,219,1,2,0,94,11,2,0,95,11,2,0,96,11,2,0,97,11,2,0,98,11,7,0,99,11,7,0,100,11,7,0,101,11,7,0,102,11,0,0,33,0,
7,0,103,11,7,0,104,11,7,0,105,11,7,0,106,11,7,0,107,11,7,0,108,11,7,0,109,11,7,0,110,11,7,0,111,11,7,0,112,11,7,0,113,11,7,0,114,11,7,0,115,11,7,0,116,11,7,0,117,11,7,0,118,11,7,0,119,11,7,0,120,11,7,0,121,11,7,0,122,11,7,0,123,11,7,0,124,11,7,0,125,11,7,0,126,11,231,0,149,7,7,0,127,11,4,0,225,4,7,0,128,11,7,0,129,11,7,0,130,11,
0,0,120,0,7,0,131,11,0,0,99,0,52,0,132,11,7,0,133,11,0,0,2,0,159,0,5,0,67,0,85,3,7,0,134,11,7,0,135,11,2,0,31,0,0,0,49,0,2,3,1,0,7,0,37,4,3,3,2,0,230,0,163,4,22,0,164,4,4,3,54,0,4,0,115,3,4,0,136,11,5,3,137,11,6,3,138,11,0,0,179,2,0,0,139,11,2,0,140,11,7,0,141,11,0,0,142,11,7,0,143,11,7,0,144,11,7,0,145,11,
7,0,146,11,7,0,30,3,7,0,31,3,7,0,9,3,7,0,25,3,7,0,29,3,2,0,219,4,0,0,147,11,2,0,148,11,7,0,149,11,7,0,150,11,0,0,151,11,0,0,252,0,0,0,217,3,0,0,152,11,2,3,153,11,4,0,154,11,4,0,224,4,7,0,155,11,7,0,156,11,7,0,157,11,7,0,158,11,2,0,159,11,2,0,160,11,2,0,161,11,2,0,162,11,2,0,163,11,2,0,164,11,2,0,165,11,2,0,166,11,
7,3,167,11,7,0,168,11,7,0,169,11,3,3,170,11,230,0,163,4,22,0,164,4,67,0,171,11,159,0,58,3,7,0,172,11,7,0,173,11,7,0,174,11,4,0,175,11,8,3,5,0,8,3,27,0,8,3,28,0,0,0,32,0,0,0,31,0,0,0,238,0,9,3,5,0,9,3,27,0,9,3,28,0,0,0,32,0,0,0,31,0,0,0,238,0,180,1,1,0,7,0,176,11,10,3,5,0,2,0,177,11,2,0,172,1,7,0,166,6,
0,0,40,8,0,0,47,2,11,3,5,0,10,0,178,11,10,0,179,11,1,0,180,11,1,0,181,11,1,0,22,0,12,3,3,0,67,0,182,11,67,0,183,11,11,3,80,0,52,0,108,0,25,0,81,0,98,0,122,1,13,3,184,11,2,0,29,0,2,0,15,6,4,0,185,11,4,0,186,11,4,0,187,11,0,0,14,6,52,0,136,0,52,0,243,9,52,0,188,11,52,0,189,11,52,0,190,11,73,0,254,0,62,0,244,0,62,0,191,11,
55,0,192,11,11,0,22,1,14,1,228,9,44,0,200,0,41,0,140,0,11,0,126,1,22,0,255,0,22,0,171,4,22,0,193,11,22,0,194,11,22,0,72,1,22,0,195,11,22,0,196,11,4,0,248,0,4,0,197,11,130,0,62,4,0,0,198,11,4,0,70,4,4,0,199,11,7,0,148,0,7,0,200,11,7,0,149,0,7,0,201,11,7,0,202,11,7,0,123,6,7,0,203,11,7,0,151,0,7,0,204,11,7,0,152,0,7,0,205,11,
7,0,153,0,7,0,206,11,7,0,119,6,7,0,159,0,4,0,50,7,2,0,31,0,2,0,207,11,2,0,208,11,2,0,129,0,2,0,219,3,2,0,223,3,2,0,209,11,0,0,83,7,0,0,210,11,2,0,211,11,2,0,212,11,2,0,213,11,2,0,214,11,2,0,154,0,0,0,215,11,0,0,216,11,2,0,222,2,0,0,13,3,0,0,217,11,7,0,218,11,7,0,219,11,2,0,99,1,2,0,220,11,0,0,99,0,7,0,107,8,
2,0,221,11,2,0,14,8,2,0,222,11,0,0,223,11,0,0,224,11,22,0,127,0,22,0,225,11,22,0,226,11,22,0,227,11,1,3,228,11,4,3,229,11,67,0,230,11,220,1,231,11,22,0,232,11,14,3,233,11,15,3,234,11,7,0,235,11,149,0,236,11,0,0,237,11,0,0,238,11,0,0,239,11,1,0,240,11,7,0,241,11,7,0,242,11,7,0,243,11,38,0,231,0,10,3,68,8,84,1,244,11,12,3,245,11,100,1,246,11,
16,3,54,0,17,3,14,0,17,3,27,0,17,3,28,0,52,0,136,0,7,0,119,6,7,0,135,7,7,0,120,6,7,0,219,1,0,0,32,0,4,0,216,8,4,0,217,8,4,0,247,11,2,0,29,0,2,0,142,4,7,0,121,6,18,3,5,0,2,0,29,0,2,0,76,10,2,0,31,0,2,0,248,11,25,0,55,1,19,3,3,0,4,0,23,7,4,0,249,11,18,3,22,1,36,0,4,0,4,0,45,1,4,0,250,11,11,0,22,1,
114,0,190,1,20,3,6,0,7,0,91,0,7,0,35,1,7,0,140,1,2,0,81,8,0,0,49,0,7,0,251,11,21,3,5,0,7,0,91,0,7,0,46,11,7,0,252,11,7,0,253,11,7,0,35,1,22,3,5,0,52,0,254,11,125,0,34,0,7,0,12,3,7,0,255,11,0,0,33,0,23,3,3,0,7,0,0,12,4,0,1,12,4,0,2,12,24,3,7,0,4,0,3,12,4,0,3,8,4,0,4,12,7,0,5,12,7,0,6,12,
7,0,7,12,0,0,33,0,25,3,8,0,25,3,27,0,25,3,28,0,52,0,198,1,4,0,14,9,2,0,31,0,2,0,248,0,7,0,35,1,7,0,8,12,26,3,7,0,26,3,27,0,26,3,28,0,52,0,198,1,2,0,88,6,2,0,31,0,2,0,99,1,0,0,40,0,27,3,19,0,21,3,9,12,21,3,10,12,20,3,11,12,21,3,253,4,22,3,12,12,4,0,224,4,7,0,35,1,7,0,222,4,7,0,13,12,4,0,3,12,
4,0,14,12,7,0,6,12,7,0,7,12,7,0,45,1,7,0,15,12,0,0,2,0,4,0,16,12,2,0,31,0,2,0,17,12,28,3,17,0,7,0,27,4,7,0,18,12,7,0,0,12,7,0,19,12,7,0,20,12,7,0,21,12,7,0,22,12,7,0,23,12,7,0,24,12,7,0,25,12,7,0,26,12,7,0,27,12,7,0,28,12,4,0,31,0,4,0,29,12,2,0,205,0,0,0,22,0,29,3,147,0,25,0,81,0,98,0,122,1,
127,0,30,12,28,3,15,5,159,0,58,3,67,0,171,11,4,0,31,0,0,0,120,0,2,0,29,0,2,0,237,3,2,0,31,12,2,0,233,7,2,0,32,12,2,0,154,0,2,0,33,12,2,0,34,12,4,0,35,12,7,0,36,12,2,0,37,12,2,0,38,12,0,0,99,0,2,0,39,12,2,0,197,5,2,0,40,12,2,0,41,12,2,0,42,12,2,0,43,12,2,0,44,12,2,0,45,12,2,0,46,12,2,0,248,4,2,0,244,4,
2,0,205,10,2,0,47,12,2,0,48,12,2,0,96,11,2,0,97,11,2,0,49,12,2,0,50,12,2,0,51,12,2,0,52,12,7,0,53,12,7,0,54,12,7,0,55,12,7,0,56,12,7,0,57,12,7,0,58,12,7,0,59,12,7,0,221,4,7,0,88,1,7,0,222,4,7,0,230,4,7,0,60,12,7,0,61,12,7,0,62,12,7,0,63,12,7,0,64,12,7,0,65,12,4,0,223,4,4,0,220,4,4,0,66,12,4,0,67,12,
2,0,68,12,0,0,242,0,7,0,226,4,7,0,227,4,7,0,228,4,7,0,69,12,7,0,70,12,7,0,71,12,7,0,72,12,7,0,73,12,7,0,74,12,7,0,75,12,7,0,76,12,7,0,77,12,7,0,14,3,7,0,45,1,7,0,78,12,7,0,211,1,7,0,79,12,7,0,80,12,7,0,81,12,7,0,82,12,4,0,83,12,0,0,77,5,4,0,84,12,4,0,85,12,7,0,98,3,7,0,86,12,7,0,87,12,7,0,88,12,
7,0,89,12,7,0,90,12,7,0,91,12,7,0,124,11,7,0,122,11,7,0,123,11,7,0,92,12,7,0,93,12,4,0,94,12,0,0,252,5,7,0,95,12,7,0,96,12,7,0,97,12,7,0,98,12,7,0,99,12,7,0,100,12,7,0,101,12,7,0,102,12,7,0,103,12,7,0,104,12,7,0,105,12,7,0,106,12,7,0,107,12,7,0,108,12,7,0,109,12,7,0,110,12,7,0,111,12,7,0,112,12,4,0,113,12,4,0,114,12,
129,0,115,12,129,0,116,12,7,0,117,12,7,0,118,12,133,0,241,7,67,0,230,11,22,0,119,12,67,0,19,5,52,0,120,12,52,0,121,12,73,0,254,0,1,3,228,11,1,3,122,12,2,0,123,12,0,0,124,12,2,0,125,12,0,0,101,5,7,0,126,12,0,0,127,12,7,0,94,11,7,0,128,12,7,0,129,12,7,0,130,12,129,0,131,12,11,0,132,12,234,0,55,0,234,0,27,0,234,0,28,0,29,3,133,12,27,3,134,12,
24,3,137,0,30,3,135,12,11,0,136,12,31,3,137,12,31,3,138,12,22,0,139,12,22,0,140,12,202,1,141,12,248,0,142,12,248,0,143,12,52,0,144,12,27,1,145,12,52,0,136,0,22,0,210,3,0,0,32,0,7,0,252,4,7,0,203,1,7,0,146,12,7,0,147,12,4,0,225,4,4,0,148,12,4,0,31,0,4,0,223,4,4,0,149,12,4,0,150,12,4,0,151,12,4,0,152,12,4,0,69,0,2,0,153,12,2,0,154,12,
2,0,155,12,0,0,252,0,0,0,156,12,0,0,99,0,2,0,157,12,2,0,158,12,2,0,159,12,0,0,160,12,230,0,163,4,22,0,164,4,22,0,161,12,23,3,162,12,4,0,163,12,4,0,164,12,32,3,165,12,207,1,240,8,33,3,166,12,7,0,167,12,7,0,168,12,11,0,116,4,234,0,169,12,34,3,5,0,34,3,27,0,34,3,28,0,4,0,29,0,4,0,170,12,11,0,22,1,35,3,8,0,35,3,27,0,35,3,28,0,
4,0,186,2,4,0,115,3,4,0,146,9,4,0,31,0,11,0,171,12,22,0,172,12,230,0,25,0,230,0,27,0,230,0,28,0,4,0,31,0,4,0,14,0,4,0,173,12,4,0,174,12,4,0,175,12,4,0,176,12,4,0,177,12,4,0,178,12,0,0,2,0,4,0,115,3,4,0,99,1,2,0,62,3,0,0,40,0,0,0,32,0,0,0,179,12,0,0,11,6,0,0,127,10,0,0,180,12,4,0,181,12,0,0,120,0,22,0,182,12,
30,3,135,12,11,0,136,12,36,3,13,0,25,0,81,0,98,0,122,1,4,0,31,0,4,0,115,3,117,0,126,4,221,0,75,8,4,0,133,4,4,0,144,7,130,0,62,4,2,0,70,4,2,0,183,12,37,3,54,0,11,0,116,4,38,3,3,0,230,0,163,4,22,0,164,4,39,3,54,0,40,3,14,0,159,0,58,3,67,0,85,3,52,0,184,12,67,0,185,12,0,0,2,0,7,0,186,12,38,3,170,11,230,0,163,4,22,0,164,4,
4,0,187,12,2,0,188,12,2,0,189,12,4,0,31,0,7,0,24,3,14,3,18,0,2,0,29,0,2,0,94,11,4,0,31,0,4,0,190,12,2,0,191,12,0,0,49,0,7,0,14,3,7,0,78,3,7,0,192,12,7,0,193,12,7,0,194,12,7,0,195,12,7,0,196,12,7,0,197,12,7,0,198,12,7,0,199,12,0,0,120,0,41,3,170,11,15,3,37,0,52,0,200,12,52,0,201,12,2,0,29,0,2,0,189,12,4,0,31,0,
7,0,202,12,0,0,203,12,0,0,18,0,7,0,204,12,7,0,205,12,7,0,206,12,7,0,207,12,7,0,208,12,7,0,209,12,7,0,210,12,7,0,211,12,7,0,212,12,7,0,213,12,7,0,214,12,7,0,215,12,7,0,216,12,7,0,217,12,7,0,218,12,7,0,219,12,7,0,220,12,7,0,221,12,7,0,222,12,7,0,223,12,7,0,224,12,7,0,225,12,7,0,226,12,7,0,227,12,7,0,228,12,7,0,229,12,7,0,230,12,
7,0,231,12,11,0,232,12,42,3,20,0,4,0,29,0,4,0,233,12,4,0,234,12,4,0,235,12,4,0,236,12,4,0,237,12,4,0,238,12,7,0,239,12,4,0,240,12,4,0,241,12,4,0,172,1,4,0,242,12,4,0,243,12,4,0,244,12,4,0,245,12,4,0,246,12,4,0,247,12,4,0,248,12,4,0,249,12,0,0,33,0,43,3,9,0,4,0,250,12,7,0,251,12,7,0,252,12,7,0,253,12,4,0,254,12,2,0,31,0,
0,0,49,0,7,0,218,1,0,0,99,0,44,3,15,0,44,3,27,0,44,3,28,0,0,0,32,0,130,0,68,7,86,1,69,7,4,0,50,7,4,0,255,12,4,0,0,13,4,0,61,7,4,0,62,7,4,0,1,13,4,0,67,7,7,0,63,7,23,0,126,0,253,0,2,13,45,3,6,0,45,3,27,0,45,3,28,0,0,0,32,0,0,0,3,13,4,0,4,13,0,0,99,0,72,1,5,0,2,0,31,0,0,0,5,13,0,0,6,13,
0,0,7,13,0,0,18,0,97,2,22,0,0,0,8,13,0,0,51,1,0,0,9,13,0,0,31,0,0,0,157,8,0,0,10,13,0,0,11,13,0,0,12,13,2,0,13,13,2,0,14,13,7,0,15,13,0,0,16,13,0,0,17,13,0,0,18,13,0,0,2,0,0,0,5,7,72,1,19,13,0,0,20,13,0,0,9,1,171,0,21,13,172,0,22,13,173,0,23,13,46,3,17,0,97,2,121,10,0,0,178,2,2,0,71,4,2,0,222,1,
2,0,193,12,2,0,31,0,7,0,24,13,7,0,25,13,4,0,26,13,0,0,27,13,0,0,28,13,0,0,29,13,0,0,30,13,0,0,31,13,0,0,32,13,0,0,2,0,52,0,33,13,47,3,91,0,97,2,121,10,42,3,34,13,4,0,203,1,4,0,24,1,4,0,25,1,7,0,35,13,4,0,36,13,4,0,37,13,4,0,38,13,4,0,39,13,2,0,31,0,2,0,47,11,7,0,40,13,4,0,41,13,2,0,42,13,2,0,45,1,
4,0,43,13,4,0,44,13,4,0,45,13,4,0,46,13,2,0,9,13,2,0,8,13,2,0,47,13,2,0,157,8,0,0,48,13,0,0,49,13,4,0,50,13,4,0,248,0,2,0,51,13,0,0,52,13,0,0,53,13,84,0,54,13,22,0,181,2,2,0,55,13,0,0,95,1,7,0,56,13,7,0,57,13,7,0,58,13,7,0,59,13,7,0,60,13,7,0,61,13,4,0,62,13,7,0,63,13,2,0,175,9,2,0,64,13,2,0,65,13,
2,0,66,13,2,0,67,13,0,0,68,13,7,0,69,13,7,0,70,13,0,0,71,13,4,0,72,13,2,0,73,13,0,0,218,6,0,0,74,13,7,0,75,13,7,0,76,13,0,0,77,13,0,0,78,13,0,0,79,13,0,0,80,13,2,0,81,13,2,0,82,13,2,0,83,13,7,0,84,13,7,0,85,13,7,0,86,13,4,0,87,13,7,0,88,13,0,0,89,13,0,0,253,1,2,0,90,13,46,3,91,13,4,0,92,13,2,0,93,13,
2,0,144,7,22,0,8,7,2,0,94,13,2,0,5,7,2,0,95,13,2,0,96,13,7,0,97,13,4,0,98,13,129,0,99,13,4,0,100,13,4,0,101,13,4,0,102,13,4,0,103,13,4,0,104,13,0,0,105,13,48,3,7,0,48,3,27,0,48,3,28,0,4,0,186,2,0,0,32,0,4,0,31,0,52,0,19,4,23,0,126,0,49,3,4,0,4,0,106,13,2,0,59,2,0,0,49,0,111,0,107,13,50,3,4,0,50,3,27,0,
50,3,28,0,0,0,4,0,111,0,108,13,51,3,2,0,111,0,109,13,22,0,110,13,52,3,15,0,132,0,110,9,111,0,108,13,132,0,111,13,111,0,112,13,51,3,113,13,139,0,114,13,129,0,115,13,11,0,116,13,0,0,117,13,4,0,172,1,4,0,118,13,4,0,119,13,7,0,120,13,0,0,99,0,49,3,80,0,53,3,17,0,52,3,121,13,2,0,31,0,2,0,122,13,2,0,123,13,2,0,124,13,2,0,125,13,4,0,248,0,
148,0,126,13,148,0,127,13,148,0,161,4,7,0,128,13,7,0,129,13,4,0,19,8,0,0,2,0,7,0,130,13,7,0,131,13,0,0,99,0,54,3,4,0,0,0,132,13,0,0,47,2,148,0,133,13,149,0,134,13,55,3,6,0,2,0,45,1,2,0,14,0,2,0,135,13,2,0,88,6,4,0,31,0,7,0,46,1,56,3,15,0,2,0,31,0,2,0,136,13,2,0,137,13,2,0,138,13,55,3,139,13,11,0,140,13,7,0,141,13,
0,0,33,0,4,0,142,13,4,0,143,13,4,0,41,12,4,0,144,13,242,0,220,6,52,0,84,6,52,0,145,13,57,3,20,0,52,3,121,13,4,0,172,1,4,0,146,13,4,0,118,2,4,0,147,13,7,0,148,13,4,0,149,13,7,0,150,13,7,0,151,13,7,0,152,13,4,0,119,2,4,0,165,2,7,0,166,2,7,0,120,2,7,0,121,2,7,0,122,2,7,0,123,2,129,0,167,2,129,0,153,13,52,0,154,13,58,3,1,0,
52,3,121,13,59,3,5,0,129,0,155,13,4,0,45,1,7,0,46,1,12,0,115,2,0,0,47,2,60,3,3,0,52,3,121,13,4,0,31,0,4,0,248,0,61,3,3,0,52,3,121,13,4,0,31,0,0,0,2,0,62,3,3,0,52,3,121,13,4,0,31,0,0,0,2,0,63,3,3,0,52,3,121,13,4,0,31,0,0,0,2,0,64,3,4,0,52,3,121,13,0,0,31,0,0,0,18,0,4,0,147,13,65,3,10,0,0,0,156,13,
0,0,157,13,0,0,158,13,0,0,29,0,0,0,99,0,7,0,143,6,7,0,159,13,7,0,65,2,7,0,28,6,52,0,160,13,66,3,8,0,11,0,140,13,4,0,31,0,4,0,161,13,7,0,162,13,0,0,2,0,129,0,163,13,129,0,164,13,65,3,165,13,67,3,1,0,129,0,166,13,68,3,37,0,4,0,45,1,7,0,153,2,7,0,71,2,7,0,140,1,7,0,69,2,7,0,80,2,4,0,23,2,7,0,70,2,129,0,32,2,
129,0,33,2,129,0,34,2,4,0,4,2,4,0,31,0,0,0,2,0,7,0,167,13,7,0,168,13,4,0,169,13,7,0,170,13,4,0,171,13,7,0,172,13,7,0,173,13,4,0,174,13,7,0,175,13,0,0,176,13,0,0,177,13,0,0,178,13,0,0,179,13,7,0,180,13,4,0,181,13,7,0,182,13,7,0,183,13,7,0,184,13,7,0,185,13,7,0,186,13,7,0,187,13,7,0,188,13,69,3,189,13,70,3,13,0,0,0,190,13,
0,0,31,0,0,0,191,13,0,0,192,13,0,0,137,6,0,0,179,2,2,0,193,13,7,0,194,13,7,0,195,13,7,0,196,13,7,0,197,13,7,0,198,13,7,0,199,13,71,3,13,0,0,0,29,0,0,0,95,1,0,0,200,13,7,0,201,13,7,0,202,13,7,0,203,13,7,0,204,13,0,0,205,13,0,0,191,2,7,0,206,13,7,0,207,13,7,0,208,13,7,0,209,13,72,3,6,0,4,0,137,6,2,0,210,13,2,0,211,13,
4,0,212,13,4,0,213,13,4,0,214,13,73,3,103,0,64,3,215,13,64,3,216,13,57,3,184,11,59,3,217,13,60,3,218,13,61,3,219,13,62,3,220,13,63,3,221,13,58,3,222,13,7,0,223,13,7,0,224,13,0,0,225,13,0,0,226,13,0,0,142,13,0,0,227,13,0,0,228,13,0,0,229,13,0,0,230,13,0,0,231,13,7,0,232,13,4,0,233,13,7,0,234,13,0,0,235,13,2,0,236,13,0,0,237,13,0,0,238,13,
0,0,239,13,0,0,240,13,2,0,241,13,7,0,242,13,0,0,243,13,0,0,244,13,0,0,242,0,66,3,245,13,67,3,246,13,53,3,247,13,54,3,248,13,56,3,249,13,7,0,250,13,7,0,251,13,2,0,252,13,0,0,253,13,0,0,254,13,0,0,255,13,0,0,0,14,0,0,1,14,0,0,2,14,0,0,3,14,0,0,4,14,2,0,210,13,2,0,5,14,2,0,6,14,2,0,7,14,2,0,211,13,2,0,8,14,2,0,9,14,
2,0,10,14,2,0,11,14,2,0,12,14,2,0,13,14,0,0,14,14,0,0,15,14,2,0,16,14,0,0,17,14,0,0,18,14,0,0,19,14,0,0,20,14,0,0,21,14,0,0,22,14,0,0,23,14,0,0,24,14,0,0,25,14,0,0,26,14,0,0,27,14,0,0,28,14,0,0,29,14,0,0,30,14,0,0,31,14,0,0,32,14,2,0,33,14,4,0,34,14,7,0,35,14,7,0,36,14,68,3,37,14,70,3,38,14,71,3,39,14,
7,0,40,14,0,0,105,13,215,0,41,14,72,3,42,14,2,0,43,14,0,0,44,14,0,0,110,2,0,0,45,14,0,0,46,14,0,0,47,14,7,0,48,14,7,0,49,14,7,0,50,14,7,0,51,14,2,0,52,14,2,0,53,14,4,0,54,14,74,3,9,0,7,0,55,14,0,0,56,14,0,0,57,14,2,0,31,0,0,0,58,14,0,0,59,14,0,0,60,14,0,0,61,14,0,0,2,0,75,3,4,0,7,0,12,3,4,0,31,0,
4,0,62,14,0,0,33,0,76,3,4,0,7,0,63,14,7,0,64,14,7,0,65,14,7,0,66,14,77,3,10,0,7,0,67,14,7,0,68,14,7,0,69,14,7,0,70,14,7,0,71,14,4,0,72,14,0,0,73,14,0,0,74,14,0,0,22,0,78,3,75,14,79,3,6,0,7,0,76,14,7,0,77,14,7,0,78,14,4,0,79,14,4,0,31,0,4,0,80,14,80,3,52,0,4,0,31,0,4,0,81,14,4,0,82,14,4,0,83,14,
7,0,84,14,4,0,85,14,0,0,33,0,4,0,86,14,4,0,87,14,7,0,88,14,7,0,89,14,4,0,90,14,4,0,91,14,7,0,92,14,7,0,93,14,4,0,94,14,4,0,95,14,7,0,96,14,7,0,97,14,7,0,98,14,4,0,99,14,4,0,100,14,4,0,101,14,7,0,102,14,7,0,103,14,7,0,104,14,7,0,105,14,0,0,106,14,0,0,133,1,7,0,107,14,7,0,108,14,7,0,109,14,7,0,110,14,4,0,247,10,
4,0,111,14,4,0,112,14,4,0,98,13,7,0,248,10,7,0,113,14,4,0,114,14,4,0,115,14,4,0,116,14,4,0,117,14,7,0,118,14,7,0,119,14,7,0,120,14,7,0,121,14,7,0,122,14,4,0,123,14,79,3,124,14,7,0,156,6,7,0,125,14,81,3,4,0,7,0,126,14,7,0,127,14,4,0,128,14,0,0,33,0,82,3,2,0,4,0,129,14,4,0,95,3,83,3,4,0,4,0,29,0,4,0,130,14,0,0,31,0,
0,0,238,0,242,0,57,0,25,0,81,0,98,0,122,1,52,0,19,4,86,1,131,14,242,0,132,14,22,0,7,0,79,1,60,7,11,0,75,0,84,3,133,14,4,0,50,7,4,0,134,14,0,0,99,0,2,0,31,0,0,0,90,7,0,0,224,11,88,1,103,7,85,3,135,14,73,3,136,14,11,0,215,8,76,3,137,14,47,3,171,3,43,3,138,14,22,0,226,0,22,0,139,14,83,3,140,14,11,0,141,14,11,0,142,14,11,0,143,14,
11,0,144,14,11,0,145,14,56,0,146,14,0,0,147,14,4,0,148,14,22,0,149,14,74,3,150,14,14,1,228,9,150,0,205,2,75,3,151,14,11,0,152,14,226,0,153,14,226,0,154,14,171,0,21,13,172,0,22,13,173,0,155,14,40,3,156,14,38,0,231,0,22,0,157,14,67,0,158,14,23,0,159,14,4,0,160,14,4,0,161,14,77,3,162,14,80,3,72,7,81,3,163,14,82,3,164,14,86,3,54,0,11,0,165,14,241,0,24,0,
25,0,81,0,22,0,166,14,22,0,167,14,22,0,168,14,22,0,239,0,242,0,220,6,2,0,31,0,2,0,169,14,2,0,170,14,0,0,0,1,0,0,9,12,0,0,171,14,0,0,172,14,0,0,173,14,0,0,174,14,0,0,175,14,0,0,176,14,0,0,177,14,0,0,255,1,87,3,178,14,88,3,179,14,11,0,180,14,89,3,181,14,38,0,231,0,90,3,6,0,90,3,27,0,90,3,28,0,90,3,182,14,91,3,183,14,2,0,31,0,
2,0,81,8,92,3,7,0,92,3,27,0,92,3,28,0,90,3,184,14,90,3,185,14,2,0,54,13,2,0,31,0,0,0,2,0,93,3,3,0,22,0,166,14,22,0,167,14,22,0,168,14,94,3,6,0,94,3,27,0,94,3,28,0,0,0,186,14,1,0,31,0,0,0,18,0,4,0,187,14,95,3,23,0,95,3,27,0,95,3,28,0,96,3,188,14,97,3,189,14,0,0,190,14,0,0,191,14,4,0,192,14,4,0,193,14,4,0,94,10,
4,0,95,10,4,0,194,14,4,0,195,14,2,0,196,14,2,0,31,0,2,0,55,7,0,0,22,0,4,0,197,14,11,0,198,14,22,0,102,3,22,0,199,14,4,0,200,14,0,0,99,0,98,3,54,0,99,3,3,0,99,3,27,0,99,3,28,0,0,0,105,1,100,3,15,0,100,3,27,0,100,3,28,0,101,3,188,14,0,0,201,14,4,0,202,14,4,0,31,0,4,0,203,14,4,0,204,14,4,0,205,14,4,0,206,14,0,0,207,14,
4,0,208,14,4,0,209,14,23,0,73,0,102,3,210,14,103,3,2,0,4,0,211,14,4,0,212,14,104,3,4,0,104,3,27,0,104,3,28,0,0,0,105,1,103,3,9,12,105,3,5,0,105,3,27,0,105,3,28,0,0,0,32,0,7,0,213,14,0,0,2,0,106,3,6,0,106,3,27,0,106,3,28,0,0,0,214,14,2,0,222,1,2,0,39,0,4,0,215,14,107,3,6,0,2,0,216,14,2,0,217,14,2,0,218,14,2,0,219,14,
2,0,31,0,0,0,49,0,108,3,3,0,109,3,220,14,0,0,221,14,0,0,238,0,110,3,25,0,110,3,27,0,110,3,28,0,90,3,184,14,90,3,185,14,90,3,222,14,90,3,223,14,241,0,224,14,111,3,13,7,0,0,240,0,0,0,225,14,2,0,226,14,2,0,227,14,2,0,228,14,0,0,229,14,0,0,172,14,2,0,31,0,2,0,230,14,0,0,49,0,112,3,188,14,107,3,231,14,22,0,232,14,22,0,239,0,22,0,233,14,
22,0,234,14,108,3,80,0,87,3,22,0,87,3,27,0,87,3,28,0,71,0,243,0,111,3,235,14,2,0,227,14,2,0,228,14,4,0,236,14,2,0,237,14,2,0,238,14,2,0,31,0,2,0,94,10,2,0,95,10,2,0,239,14,2,0,240,14,0,0,49,0,22,0,241,14,22,0,242,14,22,0,243,14,22,0,244,14,22,0,245,14,11,0,246,14,113,3,54,0,114,3,7,0,110,0,247,14,22,0,248,14,0,0,249,14,0,0,250,14,
2,0,251,14,2,0,252,14,0,0,120,0,115,3,8,0,115,3,27,0,115,3,28,0,0,0,105,1,116,3,188,14,114,3,184,9,2,0,253,14,2,0,254,14,0,0,2,0,117,3,2,0,22,0,255,14,115,3,0,15,118,3,1,0,119,3,188,14,120,3,3,0,120,3,27,0,120,3,28,0,64,1,228,6,121,3,4,0,0,0,217,10,4,0,1,15,4,0,2,15,7,0,3,15,122,3,4,0,4,0,4,15,4,0,5,15,4,0,6,15,
4,0,7,15,123,3,7,0,7,0,8,15,7,0,9,15,7,0,209,7,7,0,210,7,7,0,208,2,7,0,10,15,4,0,206,6,124,3,9,0,4,0,31,11,7,0,170,10,7,0,171,10,7,0,172,10,7,0,167,10,7,0,2,4,7,0,168,10,4,0,31,0,0,0,2,0,125,3,10,0,0,0,220,9,0,0,11,15,64,1,228,6,2,0,221,9,2,0,157,8,2,0,12,15,2,0,13,15,2,0,14,15,0,0,15,15,0,0,59,1,
126,3,13,0,126,3,27,0,126,3,28,0,4,0,67,0,4,0,16,15,4,0,17,15,4,0,18,15,121,3,19,15,0,0,220,9,125,3,188,11,122,3,20,15,123,3,21,15,124,3,22,15,173,0,2,7,127,3,8,0,8,0,23,15,4,0,31,0,4,0,95,3,7,0,24,15,0,0,120,0,8,0,25,15,7,0,26,15,0,0,99,0,128,3,1,0,47,0,72,0,129,3,68,0,129,3,27,0,129,3,28,0,11,0,103,0,11,0,64,0,
0,0,32,0,4,0,31,0,4,0,29,0,4,0,35,0,7,0,87,1,7,0,27,15,7,0,28,15,7,0,17,15,7,0,18,15,4,0,29,15,4,0,30,15,4,0,31,15,7,0,120,10,7,0,32,15,2,0,33,15,2,0,83,7,4,0,34,15,4,0,35,15,126,3,36,15,73,0,254,0,242,0,220,6,52,0,37,15,150,0,205,2,144,1,38,15,22,0,242,6,7,0,39,15,7,0,40,15,129,3,41,15,129,3,42,15,11,0,132,12,
4,0,125,5,22,0,43,15,22,0,210,0,22,0,44,15,130,3,45,15,11,0,46,15,7,0,218,1,7,0,224,1,7,0,47,15,7,0,48,15,7,0,49,15,0,0,252,5,11,0,50,15,4,0,51,15,4,0,52,15,4,0,20,6,7,0,53,15,12,0,106,3,0,0,3,7,0,0,253,1,4,0,54,15,4,0,24,1,0,0,5,7,0,0,183,12,72,1,9,7,23,0,126,0,23,0,74,0,22,0,72,1,7,0,55,15,7,0,56,15,
127,3,57,15,4,0,58,15,0,0,105,13,128,3,80,0,131,3,6,0,131,3,27,0,131,3,28,0,22,0,59,15,22,0,60,15,129,3,61,15,4,0,62,15,132,3,5,0,132,3,27,0,132,3,28,0,0,0,32,0,4,0,99,1,4,0,31,0,133,3,3,0,133,3,27,0,133,3,28,0,129,3,63,15,134,3,6,0,135,3,64,15,136,3,65,15,137,3,66,15,138,3,67,15,139,3,68,15,140,3,69,15,85,3,19,0,22,0,70,15,
22,0,71,15,11,0,126,1,22,0,43,15,22,0,72,15,22,0,210,0,129,3,73,15,0,0,74,15,0,0,75,15,0,0,76,15,4,0,77,15,4,0,78,15,4,0,79,15,4,0,80,15,84,0,81,15,4,0,82,15,4,0,134,5,141,3,83,15,134,3,80,0,142,3,4,0,7,0,84,15,7,0,143,6,2,0,85,15,2,0,86,15,143,3,6,0,7,0,87,15,7,0,88,15,7,0,89,15,7,0,90,15,4,0,91,15,4,0,92,15,
144,3,8,0,7,0,93,15,7,0,94,15,7,0,95,15,7,0,96,15,7,0,97,15,4,0,207,8,4,0,205,10,4,0,98,15,145,3,2,0,7,0,99,15,0,0,2,0,146,3,7,0,7,0,100,15,7,0,101,15,4,0,172,1,4,0,102,15,7,0,40,15,7,0,103,15,7,0,104,15,147,3,2,0,7,0,185,10,7,0,186,10,148,3,27,0,0,0,105,15,217,0,106,15,4,0,107,15,7,0,108,15,7,0,227,5,7,0,109,15,
7,0,110,15,7,0,111,15,7,0,112,15,7,0,113,15,7,0,114,15,7,0,115,15,7,0,116,15,7,0,117,15,7,0,118,15,7,0,119,15,0,0,31,0,0,0,219,14,0,0,49,0,4,0,120,15,4,0,121,15,4,0,122,15,0,0,80,4,0,0,123,15,0,0,124,15,0,0,83,7,149,3,54,0,150,3,2,0,4,0,125,15,7,0,64,6,151,3,9,0,151,3,27,0,151,3,28,0,4,0,29,0,4,0,31,0,0,0,32,0,
4,0,126,15,4,0,127,15,129,3,128,15,144,1,129,15,152,3,3,0,151,3,60,6,124,3,130,15,7,0,131,15,153,3,2,0,151,3,60,6,129,0,132,15,154,3,2,0,151,3,60,6,129,0,132,15,155,3,3,0,151,3,60,6,7,0,133,15,7,0,80,10,156,3,1,0,151,3,60,6,157,3,3,0,151,3,60,6,7,0,134,15,0,0,2,0,158,3,9,0,151,3,60,6,7,0,160,10,7,0,48,1,7,0,166,3,7,0,113,7,
7,0,80,10,7,0,135,15,7,0,136,15,4,0,29,0,159,3,3,0,159,3,27,0,159,3,28,0,129,0,132,15,160,3,2,0,151,3,60,6,22,0,137,15,47,0,1,0,10,0,138,15,161,3,9,0,161,3,27,0,161,3,28,0,4,0,29,0,4,0,248,0,0,0,33,0,2,0,31,0,2,0,23,1,0,0,32,0,0,0,59,6,162,3,5,0,7,0,148,0,0,0,2,0,163,3,139,15,163,3,140,15,163,3,141,15,164,3,7,0,
161,3,142,15,7,0,143,15,4,0,31,0,4,0,67,7,7,0,208,2,0,0,2,0,162,3,80,0,165,3,8,0,161,3,142,15,4,0,248,0,7,0,144,15,7,0,145,15,7,0,64,6,4,0,31,0,0,0,2,0,162,3,80,0,166,3,4,0,161,3,142,15,4,0,31,0,4,0,146,15,162,3,80,0,167,3,12,0,161,3,142,15,7,0,147,15,7,0,148,15,7,0,164,8,4,0,31,0,4,0,248,0,7,0,149,15,4,0,67,7,
7,0,208,2,4,0,20,6,0,0,2,0,162,3,80,0,168,3,5,0,161,3,142,15,4,0,149,0,4,0,31,0,7,0,150,15,162,3,80,0,169,3,10,0,161,3,142,15,4,0,207,2,4,0,31,0,7,0,151,15,7,0,152,15,4,0,248,0,4,0,149,15,4,0,67,7,0,0,2,0,162,3,80,0,170,3,15,0,161,3,142,15,52,0,84,6,4,0,207,2,4,0,31,0,7,0,153,15,7,0,31,1,7,0,35,4,7,0,47,1,
4,0,215,7,7,0,43,6,7,0,208,2,4,0,149,15,4,0,67,7,0,0,2,0,162,3,80,0,171,3,7,0,161,3,142,15,52,0,84,6,4,0,31,0,4,0,27,4,7,0,143,6,4,0,154,15,162,3,80,0,172,3,8,0,161,3,142,15,7,0,31,1,7,0,35,4,7,0,47,1,4,0,215,7,4,0,31,0,0,0,2,0,162,3,80,0,130,3,22,0,25,0,81,0,0,0,82,0,36,0,83,0,11,0,194,2,36,0,155,15,
73,0,254,0,7,0,218,1,7,0,156,15,7,0,224,1,7,0,157,15,7,0,158,15,7,0,204,1,2,0,172,1,2,0,182,1,0,0,2,0,8,0,159,15,11,0,8,3,11,0,160,15,11,0,142,14,11,0,161,15,4,0,238,12,4,0,162,15,70,0,6,0,70,0,27,0,70,0,28,0,22,0,239,0,0,0,240,0,0,0,241,0,0,0,242,0,173,3,8,0,70,0,27,0,70,0,28,0,22,0,239,0,0,0,240,0,0,0,241,0,
0,0,242,0,0,0,163,15,0,0,47,2,174,3,21,0,70,0,27,0,70,0,28,0,22,0,239,0,0,0,240,0,0,0,241,0,0,0,242,0,71,0,243,0,2,0,164,15,2,0,165,15,2,0,166,15,2,0,167,15,4,0,168,15,2,0,147,10,0,0,31,0,0,0,169,15,11,0,189,1,4,0,163,1,4,0,170,15,25,0,171,15,11,0,172,15,175,3,54,0,176,3,22,0,70,0,27,0,70,0,28,0,22,0,239,0,0,0,240,0,
0,0,241,0,0,0,242,0,71,0,243,0,22,0,173,15,225,0,174,15,0,0,250,14,2,0,31,0,2,0,175,15,2,0,176,15,2,0,177,15,0,0,178,15,0,0,22,0,0,0,179,15,4,0,206,6,0,0,180,15,0,0,181,15,2,0,182,15,177,3,54,0,178,3,3,0,0,0,31,0,0,0,47,2,22,0,183,15,179,3,16,0,70,0,27,0,70,0,28,0,22,0,239,0,0,0,240,0,0,0,241,0,0,0,242,0,71,0,243,0,
66,0,184,15,2,0,248,0,2,0,250,0,4,0,31,0,7,0,185,15,7,0,186,15,4,0,187,15,0,0,2,0,178,3,80,0,180,3,11,0,70,0,27,0,70,0,28,0,22,0,239,0,0,0,240,0,0,0,241,0,0,0,242,0,2,0,250,0,2,0,31,0,0,0,2,0,66,0,184,15,71,0,243,0,181,3,2,0,4,0,31,0,0,0,33,0,182,3,2,0,4,0,31,0,0,0,33,0,183,3,2,0,4,0,31,0,0,0,33,0,
184,3,28,0,70,0,27,0,70,0,28,0,22,0,239,0,0,0,240,0,0,0,241,0,0,0,242,0,71,0,243,0,7,0,89,4,7,0,90,4,2,0,165,15,2,0,218,9,2,0,188,15,2,0,189,15,4,0,31,0,7,0,109,10,0,0,227,6,0,0,190,15,0,0,191,15,0,0,192,15,0,0,2,0,7,0,193,15,14,1,228,9,181,3,194,15,182,3,195,15,183,3,196,15,0,0,223,6,0,0,148,1,185,3,54,0,186,3,6,0,
144,1,38,15,0,0,191,15,0,0,197,15,0,0,198,15,0,0,224,11,7,0,199,15,187,3,27,0,0,0,200,15,0,0,201,15,0,0,11,15,0,0,202,15,2,0,203,15,0,0,2,0,25,0,204,15,11,0,98,1,0,0,205,15,0,0,206,15,10,0,207,15,4,0,208,15,4,0,209,15,4,0,210,15,4,0,211,15,2,0,212,15,2,0,213,15,2,0,214,15,2,0,29,0,2,0,31,0,2,0,215,15,2,0,162,14,0,0,216,15,
0,0,83,7,4,0,206,6,2,0,217,15,0,0,253,1,188,3,8,0,187,3,218,15,110,0,219,15,2,0,220,15,0,0,22,0,109,0,177,1,2,0,221,15,2,0,222,15,0,0,99,0,189,3,6,0,190,3,27,0,190,3,28,0,0,0,223,15,0,0,47,2,22,0,224,15,22,0,225,15,191,3,26,0,70,0,27,0,70,0,28,0,22,0,239,0,0,0,240,0,0,0,241,0,0,0,242,0,0,0,223,15,0,0,38,4,2,0,182,1,
4,0,212,14,187,3,226,15,188,3,227,15,11,0,188,0,192,3,228,15,22,0,229,15,22,0,230,15,22,0,231,15,193,3,232,15,88,3,233,15,88,3,234,15,194,3,189,14,2,0,235,15,2,0,236,15,2,0,237,15,2,0,238,15,195,3,54,0,196,3,2,0,4,0,31,0,7,0,239,15,197,3,37,0,70,0,27,0,70,0,28,0,22,0,239,0,0,0,240,0,0,0,241,0,0,0,242,0,148,0,240,15,149,0,204,2,170,0,241,15,
169,0,242,15,14,1,228,9,7,0,193,15,7,0,89,4,7,0,90,4,7,0,109,10,7,0,243,15,7,0,244,15,0,0,248,0,0,0,249,0,0,0,245,15,0,0,246,15,0,0,247,15,0,0,248,15,0,0,249,15,0,0,187,15,0,0,192,15,0,0,250,15,0,0,252,0,4,0,31,0,7,0,251,15,7,0,252,15,0,0,99,0,7,0,253,15,4,0,254,15,4,0,255,15,186,3,0,16,196,3,1,16,198,3,26,0,70,0,27,0,
70,0,28,0,22,0,239,0,0,0,240,0,0,0,241,0,0,0,242,0,155,0,2,16,4,0,4,15,4,0,6,15,0,0,120,0,2,0,172,1,2,0,3,16,4,0,4,16,0,0,5,16,0,0,6,16,0,0,7,16,0,0,8,16,0,0,9,16,0,0,10,16,0,0,11,16,0,0,12,16,0,0,13,16,0,0,14,16,2,0,15,16,0,0,218,6,199,3,54,0,200,3,10,0,25,0,81,0,11,0,16,16,11,0,17,16,11,0,18,16,
11,0,19,16,11,0,20,16,4,0,172,1,4,0,21,16,0,0,22,16,0,0,23,16,201,3,11,0,70,0,27,0,70,0,28,0,22,0,239,0,0,0,240,0,0,0,241,0,0,0,242,0,200,3,212,5,2,0,172,1,2,0,24,16,0,0,120,0,11,0,25,16,202,3,8,0,202,3,27,0,202,3,28,0,88,1,103,7,61,2,26,16,0,0,2,0,7,0,56,10,0,0,27,16,0,0,28,16,203,3,2,0,4,0,31,0,4,0,29,16,
204,3,29,0,70,0,27,0,70,0,28,0,22,0,239,0,0,0,240,0,0,0,241,0,0,0,242,0,71,0,243,0,25,0,55,1,25,0,31,7,2,0,31,0,0,0,30,16,0,0,83,7,7,0,89,4,7,0,90,4,7,0,109,10,22,0,31,16,88,1,32,16,88,1,103,7,0,0,33,16,4,0,34,16,2,0,35,16,0,0,36,16,0,0,37,16,88,1,38,16,14,1,228,9,0,0,192,15,0,0,148,1,203,3,1,16,205,3,54,0,
206,3,7,0,206,3,27,0,206,3,28,0,4,0,39,16,4,0,35,0,0,0,40,16,4,0,133,14,4,0,29,0,207,3,14,0,70,0,27,0,70,0,28,0,22,0,239,0,0,0,240,0,0,0,241,0,0,0,242,0,22,0,41,16,22,0,42,16,0,0,43,16,0,0,44,16,4,0,3,16,4,0,45,16,4,0,46,16,4,0,47,16,208,3,9,0,70,0,27,0,70,0,28,0,22,0,239,0,0,0,240,0,0,0,241,0,0,0,242,0,
0,0,9,1,0,0,48,16,0,0,49,16,209,3,33,0,70,0,27,0,70,0,28,0,22,0,239,0,0,0,240,0,0,0,241,0,0,0,242,0,0,0,192,15,0,0,133,1,7,0,89,4,7,0,90,4,7,0,50,16,7,0,51,16,7,0,109,10,151,0,224,9,150,0,205,2,42,2,241,15,4,0,31,0,2,0,248,0,2,0,227,6,4,0,52,16,7,0,112,15,7,0,92,1,7,0,143,6,0,0,2,0,7,0,53,16,7,0,54,16,
4,0,55,16,2,0,56,16,0,0,253,1,4,0,187,15,0,0,252,5,7,0,193,15,186,3,0,16,210,3,6,0,70,0,27,0,70,0,28,0,22,0,239,0,0,0,240,0,0,0,241,0,0,0,242,0,211,3,6,0,70,0,27,0,70,0,28,0,22,0,239,0,0,0,240,0,0,0,241,0,0,0,242,0,212,3,1,0,0,0,4,0,213,3,8,0,212,3,55,1,1,0,191,1,0,0,11,1,4,0,31,0,7,0,71,4,4,0,187,14,
0,0,57,16,214,3,54,0,215,3,1,0,4,0,58,16,216,3,1,0,4,0,29,0,217,3,10,0,216,3,7,0,0,0,33,0,218,3,59,16,215,3,60,16,4,0,61,16,1,0,62,16,1,0,3,10,1,0,63,16,0,0,64,16,4,0,65,16,219,3,6,0,216,3,55,1,213,3,66,16,4,0,67,16,4,0,31,0,4,0,187,14,4,0,68,16,220,3,17,0,70,0,27,0,70,0,28,0,22,0,239,0,0,0,240,0,0,0,241,0,
0,0,242,0,219,3,69,16,4,0,70,16,0,0,133,1,1,0,208,14,22,0,71,16,217,3,72,16,4,0,31,0,4,0,73,16,4,0,74,16,0,0,99,0,221,3,54,0,222,3,15,0,222,3,27,0,222,3,28,0,0,0,75,16,1,0,38,0,1,0,31,0,0,0,40,0,4,0,76,16,4,0,77,16,0,0,78,16,7,0,79,16,7,0,164,8,7,0,80,16,7,0,81,16,7,0,82,16,0,0,120,0,223,3,15,0,25,0,81,0,
98,0,122,1,130,3,45,15,7,0,83,16,7,0,84,16,7,0,85,16,7,0,86,16,7,0,156,15,7,0,87,16,7,0,88,16,7,0,89,16,7,0,218,1,7,0,224,1,2,0,31,0,0,0,252,0,224,3,6,0,224,3,27,0,224,3,28,0,0,0,40,16,0,0,90,16,4,0,35,0,0,0,33,0,155,0,11,0,25,0,81,0,0,0,4,0,11,0,91,16,4,0,172,1,0,0,33,0,22,0,83,4,224,3,92,16,224,3,93,16,
4,0,94,16,4,0,95,16,8,0,96,16,133,0,39,0,2,0,97,16,2,0,98,16,2,0,99,16,0,0,253,1,52,0,84,6,231,0,149,7,0,0,25,9,0,0,100,16,0,0,101,16,0,0,102,16,0,0,26,11,0,0,103,16,0,0,104,16,2,0,105,16,7,0,106,16,7,0,149,0,7,0,107,16,7,0,108,16,7,0,171,3,7,0,172,3,7,0,173,3,7,0,109,16,7,0,110,16,7,0,111,16,7,0,112,16,7,0,113,16,
7,0,114,16,7,0,90,12,7,0,81,12,7,0,115,16,7,0,116,16,7,0,117,16,7,0,118,16,7,0,119,16,7,0,120,16,7,0,121,16,7,0,122,16,7,0,123,16,7,0,124,16,134,2,28,0,2,0,31,0,2,0,118,6,7,0,125,16,7,0,27,4,2,0,209,2,0,0,40,0,2,0,222,10,2,0,223,10,4,0,241,5,52,0,84,6,4,0,14,9,2,0,126,16,2,0,127,16,0,0,226,10,0,0,120,0,11,0,128,16,
7,0,129,16,7,0,130,16,2,0,131,16,2,0,132,16,2,0,133,16,0,0,111,4,7,0,134,16,7,0,135,16,7,0,136,16,0,0,77,5,135,0,153,5,129,0,137,16,225,3,1,0,10,0,248,7,231,0,61,0,25,0,81,0,98,0,122,1,7,0,247,9,7,0,248,9,7,0,133,15,7,0,80,10,7,0,79,10,7,0,138,16,7,0,139,16,7,0,140,16,7,0,141,16,0,0,99,0,7,0,142,16,7,0,143,16,7,0,144,16,
7,0,145,16,7,0,146,16,7,0,147,16,7,0,148,16,7,0,149,16,7,0,150,16,7,0,151,16,7,0,152,16,7,0,153,16,2,0,154,16,2,0,155,16,2,0,156,16,2,0,157,16,2,0,158,16,2,0,159,16,2,0,160,16,2,0,31,0,2,0,29,0,2,0,218,4,7,0,161,16,7,0,162,16,7,0,163,16,7,0,164,16,4,0,165,16,4,0,166,16,2,0,167,16,2,0,168,16,2,0,76,1,2,0,95,3,4,0,35,0,
4,0,8,6,4,0,48,1,4,0,24,1,7,0,169,16,7,0,233,4,0,0,120,0,149,0,204,2,88,1,103,7,73,0,254,0,148,0,203,2,135,0,153,5,38,0,231,0,0,0,90,7,0,0,47,2,11,0,98,1,225,3,80,0,117,2,13,0,7,0,148,0,7,0,123,6,7,0,149,0,4,0,31,0,0,0,100,16,0,0,101,16,0,0,102,16,0,0,26,11,4,0,29,0,7,0,135,7,7,0,170,16,7,0,171,16,52,0,198,1,
118,2,9,0,135,0,172,16,7,0,133,15,7,0,80,10,7,0,79,10,4,0,31,0,7,0,173,16,7,0,199,15,4,0,43,11,0,0,2,0,226,3,12,0,2,0,174,16,0,0,95,1,7,0,175,16,2,0,176,16,2,0,177,16,2,0,178,16,2,0,179,16,2,0,180,16,0,0,40,0,7,0,181,16,7,0,182,16,4,0,183,16,227,3,18,0,227,3,27,0,227,3,28,0,0,0,32,0,226,3,184,16,226,3,185,16,226,3,186,16,
226,3,187,16,7,0,188,16,2,0,189,16,2,0,190,16,2,0,191,16,2,0,192,16,2,0,193,16,2,0,194,16,2,0,195,16,2,0,196,16,2,0,197,16,0,0,40,0,228,3,11,0,0,0,198,16,0,0,199,16,0,0,200,16,0,0,201,16,0,0,202,16,0,0,203,16,0,0,204,16,0,0,238,0,2,0,205,16,2,0,206,16,7,0,207,16,229,3,16,0,0,0,208,16,0,0,209,16,0,0,210,16,0,0,211,16,0,0,212,16,
0,0,213,16,0,0,214,16,0,0,215,16,0,0,216,16,0,0,217,16,0,0,218,16,0,0,219,16,0,0,220,16,0,0,221,16,7,0,58,2,0,0,33,0,230,3,4,0,0,0,222,16,0,0,223,16,0,0,224,16,0,0,33,0,231,3,55,0,228,3,225,16,228,3,226,16,228,3,227,16,228,3,228,16,228,3,229,16,228,3,230,16,228,3,231,16,228,3,232,16,228,3,233,16,228,3,234,16,228,3,235,16,228,3,236,16,228,3,237,16,
228,3,238,16,228,3,239,16,228,3,240,16,228,3,241,16,228,3,242,16,228,3,243,16,228,3,244,16,229,3,245,16,0,0,246,16,7,0,247,16,2,0,248,16,0,0,249,16,0,0,250,16,0,0,251,16,0,0,252,16,0,0,253,16,0,0,254,16,0,0,38,4,7,0,255,16,7,0,0,17,0,0,1,17,0,0,2,17,0,0,3,17,0,0,4,17,0,0,5,17,0,0,6,17,0,0,7,17,0,0,8,17,0,0,9,17,0,0,10,17,
0,0,11,17,0,0,12,17,0,0,13,17,0,0,14,17,0,0,15,17,0,0,16,17,0,0,17,17,0,0,18,17,0,0,77,5,7,0,19,17,7,0,20,17,0,0,99,0,232,3,2,0,0,0,21,17,0,0,223,16,233,3,241,0,0,0,223,16,0,0,22,17,0,0,23,17,0,0,11,1,0,0,24,17,0,0,202,16,0,0,25,17,0,0,222,16,0,0,26,17,0,0,27,17,0,0,28,17,0,0,29,17,0,0,30,17,0,0,31,17,
0,0,32,17,0,0,33,17,0,0,34,17,0,0,35,17,0,0,36,17,0,0,37,17,0,0,38,17,0,0,39,17,0,0,40,17,0,0,41,17,0,0,42,17,230,3,43,17,232,3,44,17,0,0,45,17,0,0,46,17,0,0,47,17,0,0,48,17,0,0,49,17,0,0,50,17,0,0,51,17,0,0,52,17,0,0,53,17,0,0,54,17,0,0,55,17,0,0,56,17,0,0,57,17,0,0,58,17,0,0,59,17,0,0,60,17,0,0,61,17,
0,0,62,17,0,0,63,17,0,0,64,17,0,0,65,17,0,0,66,17,0,0,67,17,0,0,68,17,0,0,69,17,0,0,70,17,0,0,71,17,0,0,72,17,0,0,73,17,0,0,74,17,0,0,75,17,0,0,76,17,0,0,77,17,0,0,78,17,0,0,79,17,0,0,80,17,0,0,81,17,0,0,82,17,0,0,83,17,0,0,84,17,0,0,85,17,0,0,86,17,0,0,87,17,0,0,88,17,0,0,89,17,0,0,90,17,0,0,91,17,
0,0,92,17,0,0,93,17,0,0,94,17,0,0,95,17,0,0,96,17,0,0,97,17,0,0,98,17,0,0,99,17,0,0,100,17,0,0,101,17,0,0,102,17,0,0,103,17,0,0,104,17,0,0,105,17,0,0,106,17,0,0,107,17,0,0,108,17,0,0,109,17,0,0,110,17,0,0,111,17,0,0,112,17,0,0,113,17,0,0,114,17,0,0,115,17,0,0,116,17,0,0,117,17,0,0,118,17,0,0,119,17,0,0,120,17,0,0,121,17,
0,0,122,17,0,0,123,17,0,0,124,17,0,0,125,17,0,0,126,17,0,0,127,17,0,0,128,17,0,0,129,17,0,0,130,17,0,0,131,17,0,0,132,17,0,0,133,17,0,0,134,17,0,0,135,17,0,0,136,17,0,0,203,5,0,0,137,17,0,0,138,17,0,0,139,17,0,0,140,17,0,0,141,17,0,0,142,17,0,0,143,17,0,0,144,17,0,0,119,15,0,0,145,17,0,0,146,17,0,0,147,17,0,0,148,17,0,0,124,12,
7,0,149,17,0,0,150,17,0,0,151,17,0,0,152,17,0,0,153,17,0,0,154,17,0,0,155,17,0,0,156,17,0,0,157,17,0,0,158,17,0,0,159,17,0,0,160,17,0,0,161,17,0,0,162,17,0,0,163,17,0,0,164,17,0,0,165,17,0,0,166,17,0,0,167,17,0,0,168,17,0,0,169,17,0,0,170,17,0,0,171,17,0,0,172,17,0,0,173,17,0,0,147,14,0,0,174,17,0,0,175,17,0,0,176,17,0,0,177,17,
0,0,178,17,0,0,179,17,0,0,180,17,0,0,181,17,0,0,182,17,0,0,183,17,0,0,184,17,0,0,185,17,0,0,186,17,0,0,187,17,0,0,188,17,7,0,189,17,0,0,190,17,0,0,191,17,0,0,192,17,0,0,193,17,0,0,194,17,0,0,195,17,0,0,196,17,0,0,197,17,0,0,198,17,0,0,199,17,0,0,200,17,0,0,201,17,0,0,202,17,0,0,203,17,0,0,204,17,0,0,205,17,0,0,206,17,0,0,207,17,
0,0,95,1,0,0,208,17,0,0,209,17,0,0,210,17,0,0,211,17,0,0,212,17,0,0,213,17,0,0,214,17,0,0,215,17,0,0,216,17,0,0,217,17,0,0,218,17,0,0,219,17,0,0,220,17,0,0,221,17,0,0,222,17,0,0,223,17,0,0,224,17,0,0,225,17,0,0,226,17,0,0,227,17,0,0,228,17,0,0,229,17,0,0,230,17,0,0,231,17,0,0,232,17,0,0,233,17,0,0,234,17,0,0,235,17,0,0,236,17,
0,0,237,17,0,0,238,17,0,0,239,17,0,0,240,17,0,0,241,17,0,0,242,17,0,0,243,17,0,0,244,17,0,0,245,17,0,0,246,17,0,0,247,17,0,0,248,17,0,0,105,13,0,0,249,17,0,0,250,17,0,0,251,17,0,0,252,17,61,0,5,0,0,0,253,17,0,0,52,17,0,0,57,17,2,0,31,0,0,0,40,0,234,3,1,0,0,0,227,5,235,3,1,0,0,0,227,5,236,3,27,0,236,3,27,0,236,3,28,0,
0,0,32,0,0,0,178,2,231,3,254,17,233,3,255,17,233,3,0,18,233,3,1,18,233,3,2,18,233,3,3,18,233,3,4,18,233,3,5,18,233,3,6,18,233,3,7,18,233,3,8,18,233,3,9,18,233,3,10,18,233,3,11,18,233,3,12,18,233,3,13,18,233,3,14,18,233,3,15,18,233,3,16,18,61,0,17,18,234,3,18,18,235,3,19,18,4,0,20,18,237,3,3,0,4,0,221,6,7,0,199,7,7,0,135,7,238,3,23,0,
11,0,21,18,2,0,22,18,0,0,49,0,7,0,23,18,7,0,24,18,7,0,25,18,2,0,26,18,0,0,95,1,7,0,27,18,7,0,28,18,7,0,29,18,7,0,30,18,7,0,31,18,7,0,32,18,7,0,33,18,7,0,34,18,7,0,35,18,7,0,36,18,7,0,37,18,7,0,38,18,7,0,39,18,7,0,40,18,7,0,41,18,43,2,6,0,7,0,42,18,7,0,43,18,7,0,44,18,7,0,45,18,4,0,221,6,4,0,31,0,
44,2,26,0,44,2,27,0,44,2,28,0,0,0,32,0,7,0,46,18,7,0,47,18,7,0,44,18,7,0,45,18,7,0,207,2,4,0,48,18,4,0,179,2,43,2,49,18,7,0,50,18,7,0,199,7,4,0,31,0,4,0,51,18,4,0,52,18,7,0,96,0,2,0,53,18,2,0,193,12,2,0,54,18,2,0,55,18,4,0,56,18,7,0,57,18,14,1,228,9,7,0,140,1,7,0,58,18,239,3,3,0,7,0,59,18,4,0,221,6,
4,0,31,0,240,3,12,0,240,3,27,0,240,3,28,0,0,0,32,0,44,2,60,18,4,0,61,18,0,0,2,0,239,3,49,18,4,0,48,18,4,0,31,0,148,0,240,15,7,0,62,18,4,0,63,18,241,3,21,0,2,0,64,18,2,0,65,18,7,0,66,18,2,0,67,18,2,0,68,18,2,0,69,18,2,0,70,18,2,0,71,18,2,0,72,18,7,0,88,9,2,0,73,18,2,0,208,1,4,0,74,18,4,0,75,18,4,0,76,18,
4,0,77,18,7,0,139,1,4,0,78,18,4,0,79,18,7,0,80,18,7,0,81,18,242,3,16,0,4,0,31,0,4,0,82,18,4,0,83,18,4,0,84,18,4,0,85,18,7,0,86,18,44,2,87,18,4,0,88,18,7,0,89,18,7,0,90,18,7,0,92,1,7,0,91,18,7,0,92,18,7,0,93,18,4,0,206,6,4,0,179,2,243,3,5,0,4,0,31,0,7,0,199,7,4,0,94,18,4,0,95,18,237,3,96,18,244,3,12,0,
244,3,27,0,244,3,28,0,0,0,32,0,4,0,31,0,7,0,92,1,22,0,97,18,22,0,98,18,44,2,99,18,240,3,100,18,243,3,101,18,4,0,74,18,4,0,75,18,245,3,1,0,0,0,102,18,246,3,11,0,246,3,27,0,246,3,28,0,44,2,243,9,0,0,2,0,0,0,32,0,4,0,103,18,4,0,122,4,4,0,104,18,4,0,105,18,4,0,106,18,4,0,107,18,247,3,6,0,247,3,27,0,247,3,28,0,4,0,108,18,
4,0,94,0,4,0,95,0,0,0,2,0,248,3,7,0,4,0,145,3,2,0,109,18,2,0,31,0,22,0,110,18,22,0,210,0,4,0,111,18,0,0,2,0,41,2,13,0,241,3,184,9,238,3,112,18,22,0,97,18,22,0,98,18,243,3,101,18,242,3,113,18,44,2,114,1,240,3,114,18,22,0,115,18,4,0,116,18,4,0,117,18,245,3,59,7,248,3,118,18,249,3,4,0,249,3,27,0,249,3,28,0,0,0,119,18,23,0,126,0,
250,3,5,0,250,3,27,0,250,3,28,0,0,0,120,18,0,0,31,0,0,0,238,0,251,3,6,0,251,3,27,0,251,3,28,0,0,0,121,18,0,0,238,0,0,0,122,18,22,0,123,18,252,3,5,0,252,3,27,0,252,3,28,0,0,0,124,18,0,0,29,0,0,0,238,0,253,3,6,0,252,3,1,10,0,0,125,18,23,0,126,0,0,0,126,18,0,0,127,18,0,0,238,0,254,3,2,0,252,3,1,10,0,0,128,18,255,3,5,0,
252,3,1,10,0,0,129,18,0,0,130,18,4,0,131,18,0,0,33,0,0,4,7,0,0,4,27,0,0,4,28,0,0,0,32,0,0,0,127,10,2,0,132,18,2,0,31,0,0,0,33,0,1,4,10,0,1,4,27,0,1,4,28,0,0,0,32,0,0,0,133,18,0,0,134,18,0,0,135,18,0,0,136,18,1,0,31,0,1,0,209,2,0,0,242,0,2,4,5,0,4,0,31,0,7,0,137,18,7,0,107,8,7,0,138,18,7,0,37,4,
3,4,8,0,7,0,139,18,7,0,140,18,7,0,141,18,7,0,142,18,7,0,143,18,7,0,144,18,2,0,31,0,0,0,242,0,4,4,2,0,0,0,145,18,0,0,238,0,5,4,3,0,0,0,146,18,0,0,31,0,0,0,242,0,6,4,9,0,4,0,147,18,4,0,212,15,4,0,148,18,4,0,216,15,4,0,31,0,4,0,95,3,10,0,207,15,4,0,149,18,4,0,150,18,7,4,21,0,0,0,151,18,0,0,152,18,0,0,153,18,
0,0,154,18,0,0,155,18,0,0,156,18,0,0,157,18,0,0,158,18,0,0,159,18,0,0,160,18,0,0,161,18,0,0,162,18,0,0,163,18,0,0,164,18,0,0,165,18,0,0,166,18,0,0,167,18,0,0,168,18,0,0,169,18,0,0,170,18,0,0,2,0,8,4,4,0,8,4,27,0,8,4,28,0,0,0,32,0,0,0,171,18,9,4,4,0,9,4,27,0,9,4,28,0,0,0,172,18,22,0,248,14,10,4,174,0,4,0,173,18,
4,0,174,18,4,0,31,0,4,0,175,18,0,0,176,18,0,0,177,18,0,0,178,18,0,0,179,18,0,0,180,18,0,0,181,18,0,0,182,18,0,0,183,18,0,0,184,18,0,0,185,18,0,0,186,18,0,0,187,18,0,0,188,18,0,0,189,18,0,0,190,18,0,0,191,18,4,0,192,18,2,0,193,18,2,0,194,18,2,0,195,18,2,0,196,18,0,0,40,0,0,0,197,18,0,0,198,18,4,0,199,18,0,0,200,18,0,0,201,18,
0,0,202,18,0,0,203,18,0,0,204,18,2,0,205,18,4,0,206,18,4,0,207,18,4,0,208,18,4,0,209,18,4,0,210,18,7,0,211,18,4,0,212,18,4,0,213,18,7,0,214,18,7,0,215,18,7,0,216,18,4,0,217,18,4,0,41,16,0,0,218,18,0,0,219,18,2,0,220,18,2,0,221,18,2,0,222,18,0,0,223,18,22,0,224,18,22,0,225,18,22,0,226,18,22,0,227,18,22,0,228,18,22,0,229,18,22,0,230,18,
22,0,231,18,22,0,232,18,22,0,233,18,22,0,234,18,22,0,235,18,0,0,236,18,2,0,237,18,2,0,238,18,0,0,239,18,1,0,240,18,1,0,241,18,0,0,242,18,2,0,243,18,4,0,244,18,7,0,245,18,2,0,246,18,2,0,247,18,2,0,248,18,2,0,249,18,0,0,250,18,2,4,251,18,7,0,252,18,0,0,192,15,0,0,253,18,0,0,254,18,0,0,255,18,2,0,0,19,2,0,1,19,2,0,2,19,2,0,3,19,
2,0,4,19,2,0,5,19,4,0,6,19,4,0,7,19,7,0,8,19,0,0,178,5,2,0,9,19,2,0,10,19,2,0,11,19,2,0,12,19,2,0,13,19,2,0,14,19,0,0,15,19,0,0,16,19,0,0,17,19,0,0,18,19,0,0,19,19,4,0,20,19,7,0,21,19,4,0,22,19,4,0,23,19,4,0,24,19,2,0,25,19,2,0,26,19,0,0,27,19,2,0,28,19,2,0,29,19,2,0,30,19,2,0,31,19,2,0,32,19,
7,0,33,19,7,0,34,19,7,0,35,19,7,0,36,19,7,0,37,19,4,0,38,19,1,0,39,19,0,0,40,19,2,0,41,19,7,0,42,19,2,0,253,13,2,0,43,19,2,0,44,19,0,0,45,19,2,0,46,19,0,0,47,19,0,0,48,19,7,0,49,19,7,0,50,19,135,0,51,19,7,0,52,19,7,0,53,19,0,0,54,19,0,0,55,19,0,0,56,19,0,0,57,19,0,0,58,19,0,0,59,19,4,0,60,19,7,0,61,19,
2,0,62,19,2,0,63,19,2,0,64,19,2,0,65,19,2,0,66,19,2,0,67,19,4,0,68,19,0,0,69,19,0,0,73,14,0,0,70,19,0,0,71,19,2,0,72,19,2,0,83,7,7,0,73,19,0,0,74,19,0,0,75,19,0,0,76,19,0,0,77,19,3,4,78,19,5,4,79,19,6,4,80,19,7,4,81,19,4,4,80,0,109,0,6,0,4,0,82,19,3,0,83,19,3,0,84,19,1,0,85,19,1,0,86,19,1,0,87,19,
91,3,2,0,2,0,112,3,2,0,113,3,11,4,2,0,7,0,112,3,7,0,113,3,12,4,2,0,4,0,112,3,4,0,113,3,13,4,3,0,4,0,112,3,4,0,113,3,4,0,220,5,14,4,3,0,7,0,112,3,7,0,113,3,7,0,220,5,15,4,4,0,7,0,112,3,7,0,113,3,7,0,220,5,7,0,55,4,16,4,1,0,7,0,88,19,111,3,4,0,4,0,4,4,4,0,142,3,4,0,5,4,4,0,143,3,84,0,4,0,
7,0,4,4,7,0,142,3,7,0,5,4,7,0,143,3,48,0,4,0,7,0,151,0,7,0,89,19,7,0,90,19,7,0,91,19,217,0,5,0,25,0,81,0,0,0,82,0,17,4,22,1,36,0,83,0,36,0,92,19,71,0,27,0,84,0,176,3,84,0,125,3,111,3,93,19,111,3,94,19,111,3,95,19,7,0,96,19,7,0,97,19,7,0,98,19,7,0,99,19,2,0,100,19,2,0,101,19,2,0,102,19,2,0,103,19,2,0,104,19,
2,0,31,0,2,0,219,14,2,0,227,14,2,0,228,14,2,0,105,19,2,0,106,19,2,0,187,15,0,0,107,19,0,0,108,19,0,0,49,0,7,0,109,19,18,4,110,19,88,3,111,19,19,4,47,0,7,0,112,19,7,0,113,19,7,0,114,19,7,0,115,19,7,0,116,19,7,0,117,19,7,0,118,19,7,0,119,19,7,0,120,19,7,0,121,19,180,1,122,19,19,4,123,19,20,4,124,19,21,4,110,19,88,3,111,19,7,0,125,19,
7,0,126,19,7,0,127,19,7,0,128,19,7,0,129,19,7,0,130,19,7,0,139,1,7,0,131,19,7,0,132,19,7,0,133,19,7,0,106,16,7,0,134,19,0,0,135,19,0,0,136,19,0,0,227,6,0,0,137,19,0,0,138,19,0,0,139,19,0,0,140,19,0,0,255,1,7,0,141,19,2,0,142,19,2,0,143,19,7,0,144,19,0,0,145,19,0,0,146,19,0,0,147,19,0,0,127,12,0,0,38,19,7,0,148,19,7,0,149,19,
7,0,150,19,84,3,7,0,7,0,28,6,7,0,151,19,7,0,10,11,7,0,152,19,7,0,153,19,2,0,58,1,0,0,22,0,78,3,32,0,0,0,29,0,0,0,154,19,0,0,155,19,0,0,156,19,2,0,31,0,0,0,157,19,0,0,158,19,0,0,159,19,0,0,160,19,0,0,161,19,0,0,179,2,0,0,162,19,0,0,163,19,0,0,164,19,7,0,165,19,7,0,166,19,7,0,167,19,7,0,168,19,7,0,169,19,7,0,170,19,
7,0,171,19,7,0,172,19,7,0,173,19,7,0,174,19,7,0,175,19,7,0,176,19,7,0,177,19,7,0,178,19,4,0,179,19,0,0,180,19,23,0,126,0,11,0,188,0,22,4,28,0,4,0,31,0,4,0,181,19,7,0,182,19,7,0,183,19,4,0,184,19,4,0,185,19,7,0,186,19,7,0,187,19,7,0,188,19,7,0,189,19,7,0,190,19,7,0,191,19,7,0,192,19,7,0,193,19,7,0,194,19,7,0,195,19,7,0,196,19,
7,0,197,19,7,0,198,19,7,0,199,19,7,0,200,19,7,0,201,19,7,0,202,19,7,0,203,19,4,0,204,19,7,0,205,19,4,0,206,19,7,0,207,19,23,4,5,0,11,0,208,19,11,0,209,19,4,0,31,0,0,0,120,0,85,1,210,19,24,4,60,0,70,0,27,0,70,0,28,0,22,0,239,0,0,0,240,0,0,0,241,0,0,0,242,0,7,0,130,19,7,0,139,1,7,0,211,19,0,0,212,19,0,0,132,1,0,0,224,11,
0,0,223,6,4,0,213,19,4,0,214,19,2,0,136,19,2,0,227,6,52,0,19,4,52,0,215,19,84,0,216,19,24,4,123,19,0,0,217,19,2,0,218,19,0,0,101,5,4,0,134,14,2,0,219,19,2,0,47,14,2,0,220,19,2,0,221,19,2,0,222,19,2,0,223,19,2,0,31,0,4,0,0,2,7,0,226,2,7,0,58,6,7,0,224,19,7,0,225,19,7,0,106,16,0,0,255,1,0,0,192,15,0,0,226,19,0,0,227,19,
0,0,228,19,0,0,229,19,0,0,230,19,0,0,231,19,2,0,232,19,2,0,233,19,7,0,234,19,14,1,228,9,2,0,235,19,0,0,236,19,0,0,144,7,7,0,237,19,7,0,238,19,7,0,239,19,78,3,75,14,22,4,1,16,218,3,59,16,23,4,80,0,25,4,5,0,25,4,27,0,25,4,28,0,4,0,29,0,0,0,2,0,0,0,240,19,26,4,2,0,25,4,7,0,25,0,55,1,27,4,3,0,25,4,7,0,4,0,241,19,
0,0,2,0,28,4,3,0,25,4,7,0,4,0,53,10,0,0,120,0,29,4,3,0,25,4,7,0,4,0,242,19,0,0,120,0,30,4,3,0,25,4,7,0,4,0,243,19,4,0,244,19,31,4,3,0,25,4,7,0,4,0,245,19,4,0,99,1,32,4,3,0,25,4,7,0,4,0,53,10,0,0,120,0,33,4,4,0,25,4,7,0,4,0,246,19,4,0,247,19,88,1,248,19,218,3,1,0,22,0,55,10,34,4,8,0,7,0,76,2,
4,0,249,19,4,0,250,19,4,0,251,19,4,0,158,5,4,0,159,5,7,0,150,5,4,0,255,1,35,4,4,0,4,0,19,0,4,0,192,3,7,0,54,1,7,0,174,5,36,4,23,0,25,0,81,0,98,0,122,1,0,0,178,2,36,0,83,0,0,0,182,2,0,0,252,19,0,0,95,1,4,0,229,0,4,0,253,19,4,0,13,1,4,0,31,0,4,0,254,19,130,0,62,4,2,0,70,4,2,0,191,2,35,4,255,19,34,4,162,14,
0,0,0,20,0,0,183,12,0,0,192,2,7,0,172,5,11,0,116,4,37,4,54,0,38,4,7,0,22,0,1,20,4,0,2,20,4,0,3,20,4,0,31,0,0,0,2,0,88,3,4,20,39,4,5,20,40,4,2,0,41,4,54,0,42,4,6,20,43,4,27,0,25,0,81,0,44,4,7,20,44,4,8,20,22,0,9,20,1,0,10,20,0,0,53,13,2,0,11,20,2,0,12,20,2,0,13,20,22,0,14,20,4,0,15,20,4,0,16,20,
22,0,17,20,22,0,18,20,22,0,19,20,22,0,20,20,45,4,21,20,45,4,22,20,45,4,23,20,22,0,24,20,88,3,25,20,0,0,26,20,0,0,148,1,46,4,27,20,47,4,28,20,40,4,29,20,48,4,54,0,44,4,46,0,44,4,27,0,44,4,28,0,11,0,30,20,11,0,31,20,44,4,136,0,242,0,220,6,242,0,32,20,0,0,33,20,242,0,34,20,49,4,35,20,93,3,36,20,241,0,37,20,4,0,169,14,2,0,38,20,
2,0,39,20,2,0,94,10,2,0,95,10,0,0,40,20,0,0,142,4,2,0,133,14,2,0,41,20,2,0,42,20,2,0,43,20,2,0,44,20,2,0,45,20,0,0,46,20,0,0,47,20,0,0,48,20,0,0,49,20,2,0,50,20,4,0,51,20,50,4,52,20,51,4,53,20,51,4,54,20,0,0,55,20,0,0,9,1,22,0,233,14,22,0,56,20,22,0,57,20,72,1,9,7,22,0,58,20,11,0,59,20,11,0,188,0,10,0,60,20,
52,4,54,0,11,0,98,1,53,4,20,0,53,4,27,0,53,4,28,0,0,0,105,1,23,0,73,0,0,0,61,20,2,0,62,20,2,0,29,0,12,0,25,0,12,0,199,8,12,0,63,20,12,0,64,20,12,0,65,20,12,0,66,20,12,0,67,20,0,0,238,0,2,0,68,20,1,0,31,0,1,0,69,20,2,0,81,0,54,4,70,20,55,4,4,0,55,4,27,0,55,4,28,0,53,4,71,20,53,4,72,20,56,4,13,0,56,4,27,0,
56,4,28,0,22,0,123,18,22,0,73,20,0,0,105,1,2,0,74,20,2,0,75,20,0,0,76,20,2,0,31,0,2,0,77,20,57,4,78,20,57,4,79,20,11,0,80,20,58,4,4,0,58,4,27,0,58,4,28,0,0,0,105,1,23,0,126,0,45,4,8,0,45,4,27,0,45,4,28,0,0,0,105,1,0,0,81,20,22,0,82,20,4,0,83,20,2,0,31,0,0,0,40,0,193,3,14,0,193,3,27,0,193,3,28,0,0,0,105,1,
23,0,73,0,59,4,188,14,11,0,84,20,11,0,78,0,54,4,70,20,38,4,85,20,22,0,86,20,193,3,87,20,97,3,189,14,2,0,31,0,0,0,22,0,109,3,10,0,109,3,27,0,109,3,28,0,0,0,105,1,0,0,88,20,0,0,89,20,2,0,39,0,2,0,121,18,4,0,248,0,23,0,73,0,60,4,54,0,61,4,4,0,61,4,27,0,61,4,28,0,241,0,37,20,0,0,32,0,62,4,3,0,62,4,27,0,62,4,28,0,
0,0,6,6,63,4,13,0,25,0,81,0,22,0,90,20,22,0,91,20,22,0,92,20,22,0,93,20,242,0,94,20,0,0,2,0,4,0,95,20,4,0,172,1,4,0,96,20,64,4,54,0,110,0,219,15,218,3,59,16,65,4,6,0,65,4,27,0,65,4,28,0,11,0,136,0,11,0,73,10,4,0,97,20,0,0,98,20,49,4,4,0,63,4,99,20,61,4,100,20,63,4,101,20,61,4,102,20,86,1,38,0,25,0,81,0,98,0,122,1,
0,0,33,0,2,0,233,7,2,0,103,20,7,0,104,20,7,0,105,20,7,0,106,20,7,0,165,3,7,0,107,20,7,0,227,1,2,0,248,0,0,0,111,4,7,0,108,20,7,0,109,20,7,0,110,20,7,0,111,20,7,0,112,20,7,0,113,20,2,0,31,0,0,0,218,6,4,0,114,20,7,0,115,20,7,0,88,7,7,0,116,20,7,0,117,20,7,0,118,20,0,0,252,5,73,0,254,0,2,0,89,7,2,0,90,7,0,0,2,0,
38,0,231,0,88,1,103,7,84,1,244,11,11,0,75,0,22,0,66,8,10,0,248,7,42,4,15,0,78,3,75,14,7,0,119,20,0,0,18,0,0,0,120,20,52,0,121,20,7,0,122,20,7,0,123,20,0,0,124,20,0,0,125,20,0,0,253,1,7,0,126,20,7,0,127,20,4,0,31,0,4,0,213,19,4,0,214,19,66,4,3,0,66,4,27,0,66,4,28,0,0,0,128,20,67,4,10,0,67,4,27,0,67,4,28,0,0,0,32,0,
0,0,129,20,22,0,130,20,7,0,131,20,2,0,132,20,0,0,49,0,7,0,133,20,7,0,134,20,68,4,3,0,68,4,27,0,68,4,28,0,0,0,135,20,69,4,20,0,69,4,27,0,69,4,28,0,0,0,32,0,0,0,29,0,0,0,47,2,22,0,136,20,0,0,137,20,23,0,138,20,54,4,139,20,2,0,140,20,2,0,141,20,2,0,142,20,2,0,143,20,0,0,144,20,7,0,145,20,7,0,146,20,7,0,147,20,2,0,148,20,
0,0,218,6,22,0,149,20,70,4,6,0,70,4,27,0,70,4,28,0,0,0,32,0,22,0,123,18,2,0,150,20,0,0,22,0};

int bfBlenderLen=sizeof(bfBlenderFBT);
//===============================================================================================

#endif //bftBlend_imp_
#endif //FBTBLEND_IMPLEMENTATION

