// jconfig.h for libfiles' libjpeg-turbo build with GCC and Clang, carried from Diffractor's Linux
// build. libjpeg-turbo's sources reach it by a quoted include that the fetched source tree does not
// satisfy, so the recipe puts this directory on the include path. jconfigint.h forwards to it.

#define JPEG_LIB_VERSION 80
#define LIBJPEG_TURBO_VERSION 3.2.0
#define C_ARITH_CODING_SUPPORTED 1
#define D_ARITH_CODING_SUPPORTED 1
#define MEM_SRCDST_SUPPORTED 1
#define HAVE_PROTOTYPES 1
#define HAVE_UNSIGNED_CHAR 1
#define HAVE_UNSIGNED_SHORT 1
#define TRANSFORMS_SUPPORTED 1

#ifndef BITS_IN_JSAMPLE
#define BITS_IN_JSAMPLE 8
#endif

/* The jsimd routines are 8-bit only; enabling them for the 12- and 16-bit
   libraries silently substitutes 8-bit IDCT, upsample and colour convert.
   libfiles defines LIBFILES_JPEG_NO_SIMD where it builds no SIMD sources: a
   target without an assembler, or one it has no SIMD sources for. */
#if BITS_IN_JSAMPLE == 8 && !defined(LIBFILES_JPEG_NO_SIMD)
#define WITH_SIMD 1
#endif

#undef CHAR_IS_UNSIGNED
#define HAVE_STDDEF_H
#define HAVE_STDLIB_H
#undef NEED_BSD_STRINGS
#undef NEED_SYS_TYPES_H
#undef NEED_FAR_POINTERS
#undef NEED_SHORT_EXTERNAL_NAMES
#undef INCOMPLETE_TYPES_BROKEN
#undef NEON_INTRINSICS

#define PACKAGE_NAME "TurboJpeg"
#define VERSION "3.2.0"
#define BUILD __DATE__

/* Match the MSVC build's boolean width so behaviour does not differ by platform. */
typedef unsigned char boolean;
#define HAVE_BOOLEAN

#ifndef INLINE
#define INLINE __inline__ __attribute__((always_inline))
#endif

#ifndef HIDDEN
#define HIDDEN __attribute__((visibility("hidden")))
#endif

#define SIZEOF_SIZE_T __SIZEOF_SIZE_T__

/* X11 headers also define INT32; XMD_H is the flag jmorecfg.h checks. */
typedef short INT16;
typedef signed int INT32;
#define XMD_H

#ifdef JPEG_INTERNALS
#undef RIGHT_SHIFT_IS_UNSIGNED
#endif

#define FALLTHROUGH __attribute__((fallthrough));

/* getenv is used only by upstream's test harness, which is not built here. */
#define NO_GETENV

#define THREAD_LOCAL __thread

/* SIMD dispatch architecture selector (values from simd/jsimdconst.h). */
#if defined(__x86_64__)
#define SIMD_ARCHITECTURE X86_64
#elif defined(__i386__)
#define SIMD_ARCHITECTURE I386
#endif
