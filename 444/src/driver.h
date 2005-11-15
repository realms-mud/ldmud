#ifndef DRIVER_H__
#define DRIVER_H__

/*------------------------------------------------------------------
 * Global mandatory include file.
 *
 * It contains various global macros and declarations, and takes
 * care of the proper inclusion of the configuration/portability
 * include files.
 *------------------------------------------------------------------
 */

#define __DRIVER_SOURCE__

#include "config.h"

/* DEBUG: Some debug defines */

/* Activate Mapping consistency check code. It slows the mapping
 * activities down and was used to find the notorious FinalFrontier
 * mapping bug.
 */
/* #define CHECK_MAPPINGS */

/* Activate total mapping size consistency check code. It has a small
 * impact on the execution speed. This define was used to find
 * the inaccuracy in the mapping statistic.
 */
/* #define CHECK_MAPPING_TOTAL */

/* Activate object refcount check code. It will produce a decent
 * amount of log output. It will also fatal() the driver as soon
 * as it detects an inconsistency in the list of destructed objects.
 */
/* #define CHECK_OBJECT_REF */

/* Activate object referencing checking code during the GC. It will
 * print error messages to gcout when an object or program is
 * referenced as something else. No penalty for using.
 * Requires MALLOC_TRACE to work. Incompatible with DUMP_GC_REFS.
 */
#ifdef MALLOC_TRACE
#    define CHECK_OBJECT_GC_REF
#endif

/* Deactivate the tracking of blueprints. This will disable the
 * efuns blueprint(), but probably avoid the object refcount bug.
 */
/* #define NO_BLUEPRINT */

/* Activate total smalloc size consistency check code. This will produce
 * a lot of output in the GC log.
 */
/* #define CHECK_SMALLOC_TOTAL */

/* Sometimes the GC stumbles over invalid references to memory
 * blocks (namely 'Program referenced as something else'). Define
 * this macro to get a detailed dump of all found references
 * (Warning: LOTS of output!).
 */
/* #define DUMP_GC_REFS */


/* TODO: Some TODO defines */

/* NO_NEGATIVE_RANGES: If defined, assignments to negative ranges
 *   like [4..2] are not allowed. However, they are useful at times
 *   and so this switch should be under control of a pragma or special
 *   syntactic construct. For now and for compatibility reasons, these
 *   ranges remain allowed.
 */
/* #undef NO_NEGATIVE_RANGES */

/* Verify some of the definitions in config.h */

#if !defined(MALLOC_smalloc) && !defined(MALLOC_sysmalloc)
#  define MALLOC_smalloc
#endif

#if defined(MALLOC_sysmalloc)
   /* TODO: Implement allocation tracing for sysmalloc. This
    * TODO:: would also allow us a generic malloced_size().
    */
#  if defined(MALLOC_TRACE)
#    undef MALLOC_TRACE
#  endif
#  if defined(MALLOC_LPC_TRACE)
#    undef MALLOC_LPC_TRACE
#  endif
#endif

/* Do we have full GC support? */

#if defined(MALLOC_smalloc)
#  define GC_SUPPORT 1
#endif
 

/* This one is for backwards compatibility with old config.hs */

#if defined(NATIVE_MODE) && !defined(STRICT_EUIDS)
#  define STRICT_EUIDS
#elif defined(COMPAT_MODE)
#  undef STRICT_EUIDS
#endif

/* The string table is shadowed only in DEBUG mode */

#if !defined(DEBUG) && defined(CHECK_STRINGS)
#  undef CHECK_STRINGS
#endif

/* Define some macros needed in the headers included from ../mudlib/sys */

#ifdef USE_IPV6
#    define __IPV6__
#endif

/* Include the portability headers */
#include "port.h"

/* TODO: this ctype-stuff might go into lex.h (impl in efun_defs.c) */
#define _MCTe 0x01 /* escaped character in save/restore object. */
#define _MCTd 0x02 /* numeric digit                */


#define _MCTs 0x10 /* whitespace EXCLUDING '\n'        */

#define _MCTx 0x40 /* hexadecimal                */
#define _MCTa 0x80 /* alphanumeric or '_'         */
extern unsigned char _my_ctype[];
#define isescaped(c) (_my_ctype[(unsigned char)(c)]&_MCTe)
#define isalunum( c) (_my_ctype[(unsigned char)(c)]&_MCTa)
#define lexdigit( c) (_my_ctype[(unsigned char)(c)]&_MCTd)

#ifndef MAXINT
#    define MAXINT (0x7fffffffU)
#endif

/* A define to point out empty loop bodies. */
#define NOOP

/* A macro to wrap statements */
#define MACRO(x) do { x ; } while(0)

#endif /* DRIVER_H__ */