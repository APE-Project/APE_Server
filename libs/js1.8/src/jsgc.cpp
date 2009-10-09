/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Communicator client code, released
 * March 31, 1998.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

/*
 * JS Mark-and-Sweep Garbage Collector.
 *
 * This GC allocates fixed-sized things with sizes up to GC_NBYTES_MAX (see
 * jsgc.h). It allocates from a special GC arena pool with each arena allocated
 * using malloc. It uses an ideally parallel array of flag bytes to hold the
 * mark bit, finalizer type index, etc.
 *
 * XXX swizzle page to freelist for better locality of reference
 */
#include <stdlib.h>     /* for free */
#include <math.h>
#include <string.h>     /* for memset used when DEBUG */
#include "jstypes.h"
#include "jsstdint.h"
#include "jsutil.h" /* Added by JSIFY */
#include "jshash.h" /* Added by JSIFY */
#include "jsbit.h"
#include "jsclist.h"
#include "jsprf.h"
#include "jsapi.h"
#include "jsatom.h"
#include "jscntxt.h"
#include "jsversion.h"
#include "jsdbgapi.h"
#include "jsexn.h"
#include "jsfun.h"
#include "jsgc.h"
#include "jsinterp.h"
#include "jsiter.h"
#include "jslock.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jsparse.h"
#include "jsscope.h"
#include "jsscript.h"
#include "jsstaticcheck.h"
#include "jsstr.h"
#include "jstask.h"
#include "jstracer.h"

#if JS_HAS_XML_SUPPORT
#include "jsxml.h"
#endif

#ifdef INCLUDE_MOZILLA_DTRACE
#include "jsdtracef.h"
#endif

/*
 * Check if posix_memalign is available.
 */
#if _POSIX_C_SOURCE >= 200112L || _XOPEN_SOURCE >= 600 || MOZ_MEMORY
# define HAS_POSIX_MEMALIGN 1
#else
# define HAS_POSIX_MEMALIGN 0
#endif

/*
 * jemalloc provides posix_memalign.
 */
#ifdef MOZ_MEMORY
extern "C" {
#include "../../memory/jemalloc/jemalloc.h"
}
#endif

/*
 * Include the headers for mmap unless we have posix_memalign and do not
 * insist on mmap.
 */
#if JS_GC_USE_MMAP || (!defined JS_GC_USE_MMAP && !HAS_POSIX_MEMALIGN)
# if defined(XP_WIN)
#  ifndef JS_GC_USE_MMAP
#   define JS_GC_USE_MMAP 1
#  endif
#  include <windows.h>
# elif defined(__SYMBIAN32__)
// Symbian's OpenC has mmap (and #defines _POSIX_MAPPED_FILES), but
// doesn't implement MAP_ANON.  If we have MOZ_MEMORY, then we can use
// posix_memalign; we've defined HAS_POSIX_MEMALIGN above.  Otherwise,
// we overallocate.
# else
#  if defined(XP_UNIX) || defined(XP_BEOS)
#   include <unistd.h>
#  endif
#  if _POSIX_MAPPED_FILES > 0
#   ifndef JS_GC_USE_MMAP
#    define JS_GC_USE_MMAP 1
#   endif
#   include <sys/mman.h>

/* On Mac OS X MAP_ANONYMOUS is not defined. */
#   if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#    define MAP_ANONYMOUS MAP_ANON
#   endif
#   if !defined(MAP_ANONYMOUS)
#    define MAP_ANONYMOUS 0
#   endif
#  else
#   if JS_GC_USE_MMAP
#    error "JS_GC_USE_MMAP is set when mmap is not available"
#   endif
#  endif
# endif
#endif

/*
 * Check JSTempValueUnion has the size of jsval and void * so we can
 * reinterpret jsval as void* GC-thing pointer and use JSTVU_SINGLE for
 * different GC-things.
 */
JS_STATIC_ASSERT(sizeof(JSTempValueUnion) == sizeof(jsval));
JS_STATIC_ASSERT(sizeof(JSTempValueUnion) == sizeof(void *));

/*
 * Check that JSTRACE_XML follows JSTRACE_OBJECT, JSTRACE_DOUBLE and
 * JSTRACE_STRING.
 */
JS_STATIC_ASSERT(JSTRACE_OBJECT == 0);
JS_STATIC_ASSERT(JSTRACE_DOUBLE == 1);
JS_STATIC_ASSERT(JSTRACE_STRING == 2);
JS_STATIC_ASSERT(JSTRACE_XML    == 3);

/*
 * JS_IS_VALID_TRACE_KIND assumes that JSTRACE_STRING is the last non-xml
 * trace kind when JS_HAS_XML_SUPPORT is false.
 */
JS_STATIC_ASSERT(JSTRACE_STRING + 1 == JSTRACE_XML);

/*
 * Check that we can reinterpret double as JSGCDoubleCell.
 */
JS_STATIC_ASSERT(sizeof(JSGCDoubleCell) == sizeof(double));

/*
 * Check that we can use memset(p, 0, ...) to implement JS_CLEAR_WEAK_ROOTS.
 */
JS_STATIC_ASSERT(JSVAL_NULL == 0);

/*
 * Check consistency of external string constants from JSFinalizeGCThingKind.
 */
JS_STATIC_ASSERT(FINALIZE_EXTERNAL_STRING_LAST - FINALIZE_EXTERNAL_STRING0 ==
                 JS_EXTERNAL_STRING_LIMIT - 1);

/*
 * A GC arena contains a fixed number of flag bits for each thing in its heap,
 * and supports O(1) lookup of a flag given its thing's address.
 *
 * To implement this, we allocate things of the same size from a GC arena
 * containing GC_ARENA_SIZE bytes aligned on GC_ARENA_SIZE boundary. The
 * following picture shows arena's layout:
 *
 *  +------------------------------+--------------------+---------------+
 *  | allocation area for GC thing | flags of GC things | JSGCArenaInfo |
 *  +------------------------------+--------------------+---------------+
 *
 * To find the flag bits for the thing we calculate the thing index counting
 * from arena's start using:
 *
 *   thingIndex = (thingAddress & GC_ARENA_MASK) / thingSize
 *
 * The details of flag's lookup depend on thing's kind. For all GC things
 * except doubles we use one byte of flags where the 4 bits determine thing's
 * type and the rest is used to implement GC marking, finalization and
 * locking. We calculate the address of flag's byte using:
 *
 *   flagByteAddress =
 *       (thingAddress | GC_ARENA_MASK) - sizeof(JSGCArenaInfo) - thingIndex
 *
 * where
 *
 *   (thingAddress | GC_ARENA_MASK) - sizeof(JSGCArenaInfo)
 *
 * is the last byte of flags' area.
 *
 * This implies that the things are allocated from the start of their area and
 * flags are allocated from the end. This arrangement avoids a relatively
 * expensive calculation of the location of the boundary separating things and
 * flags. The boundary's offset from the start of the arena is given by:
 *
 *   thingsPerArena * thingSize
 *
 * where thingsPerArena is the number of things that the arena can hold:
 *
 *   (GC_ARENA_SIZE - sizeof(JSGCArenaInfo)) / (thingSize + 1).
 *
 * To allocate doubles we use a specialized arena. It can contain only numbers
 * so we do not need the type bits. Moreover, since the doubles do not require
 * a finalizer and very few of them are locked via js_LockGCThing API, we use
 * just one bit of flags per double to denote if it was marked during the
 * marking phase of the GC. The locking is implemented via a hash table. Thus
 * for doubles the flag area becomes a bitmap.
 *
 * JS_GC_USE_MMAP macro governs the choice of the aligned arena allocator.
 * When it is true, a platform-dependent function like mmap is used to get
 * memory aligned on CPU page boundaries. If the macro is false or undefined,
 * posix_memalign is used when available. Otherwise the code uses malloc to
 * over-allocate a chunk with js_gcArenasPerChunk aligned arenas. The
 * approximate space overhead of this is 1/js_gcArenasPerChunk. For details,
 * see NewGCChunk/DestroyGCChunk below.
 *
 * The code also allocates arenas in chunks when JS_GC_USE_MMAP is 1 to
 * minimize the overhead of mmap/munmap. In this case js_gcArenasPerChunk can
 * not be a compile-time constant as the system page size is not known until
 * runtime.
 */
#if JS_GC_USE_MMAP
static uint32 js_gcArenasPerChunk = 0;
static JSBool js_gcUseMmap = JS_FALSE;
#elif HAS_POSIX_MEMALIGN
# define js_gcArenasPerChunk 1
#else
# define js_gcArenasPerChunk 7
#endif

#if defined(js_gcArenasPerChunk) && js_gcArenasPerChunk == 1
# define CHUNKED_ARENA_ALLOCATION 0
#else
# define CHUNKED_ARENA_ALLOCATION 1
#endif

#define GC_ARENA_SHIFT              12
#define GC_ARENA_MASK               ((jsuword) JS_BITMASK(GC_ARENA_SHIFT))
#define GC_ARENA_SIZE               JS_BIT(GC_ARENA_SHIFT)

/*
 * JS_GC_ARENA_PAD defines the number of bytes to pad JSGCArenaInfo structure.
 * It is used to improve allocation efficiency when using posix_memalign. If
 * malloc's implementation uses internal headers, then calling
 *
 *   posix_memalign(&p, GC_ARENA_SIZE, GC_ARENA_SIZE * js_gcArenasPerChunk)
 *
 * in a sequence leaves holes between allocations of the size GC_ARENA_SIZE
 * due to the need to fit headers. JS_GC_ARENA_PAD mitigates that so the code
 * calls
 *
 *     posix_memalign(&p, GC_ARENA_SIZE,
 *                    GC_ARENA_SIZE * js_gcArenasPerChunk - JS_GC_ARENA_PAD)
 *
 * When JS_GC_ARENA_PAD is equal or greater than the number of words in the
 * system header, the system can pack all allocations together without holes.
 *
 * With JS_GC_USE_MEMALIGN we want at least 2 word pad unless posix_memalign
 * comes from jemalloc that does not use any headers/trailers.
 */
#ifndef JS_GC_ARENA_PAD
# if HAS_POSIX_MEMALIGN && !MOZ_MEMORY
#  define JS_GC_ARENA_PAD (2 * JS_BYTES_PER_WORD)
# else
#  define JS_GC_ARENA_PAD 0
# endif
#endif

struct JSGCArenaInfo {
    /*
     * Allocation list for the arena or NULL if the arena holds double values.
     */
    JSGCArenaList   *list;

    /*
     * Pointer to the previous arena in a linked list. The arena can either
     * belong to one of JSContext.gcArenaList lists or, when it does not have
     * any allocated GC things, to the list of free arenas in the chunk with
     * head stored in JSGCChunkInfo.lastFreeArena.
     */
    JSGCArenaInfo   *prev;

#if !CHUNKED_ARENA_ALLOCATION
    jsuword         prevUntracedPage;
#else
    /*
     * A link field for the list of arenas with marked but not yet traced
     * things. The field is encoded as arena's page to share the space with
     * firstArena and arenaIndex fields.
     */
    jsuword         prevUntracedPage :  JS_BITS_PER_WORD - GC_ARENA_SHIFT;

    /*
     * When firstArena is false, the index of arena in the chunk. When
     * firstArena is true, the index of a free arena holding JSGCChunkInfo or
     * NO_FREE_ARENAS if there are no free arenas in the chunk.
     *
     * GET_ARENA_INDEX and GET_CHUNK_INFO_INDEX are convenience macros to
     * access either of indexes.
     */
    jsuword         arenaIndex :        GC_ARENA_SHIFT - 1;

    /* Flag indicating if the arena is the first in the chunk. */
    jsuword         firstArena :        1;
#endif

    union {
        jsuword     untracedThings;     /* bitset for fast search of marked
                                           but not yet traced things */
        bool        hasMarkedDoubles;   /* the arena has marked doubles */
    } u;

#if JS_GC_ARENA_PAD != 0
    uint8           pad[JS_GC_ARENA_PAD];
#endif
};

/* GC flag definitions, must fit in 8 bits (type index goes in the low bits). */
const uint8 GCF_MARK   = JS_BIT(0);
const uint8 GCF_FINAL  = JS_BIT(1);
const uint8 GCF_LOCK   = JS_BIT(2);    /* lock request bit in API */

/*
 * The private JSGCThing struct, which describes a JSRuntime.gcFreeList element.
 */
struct JSGCThing {
    JSGCThing   *next;
    uint8       *flagp;
};

/*
 * Verify that the bit fields are indeed shared and JSGCArenaInfo is as small
 * as possible. The code does not rely on this check so if on a particular
 * platform this does not compile, then, as a workaround, comment the assert
 * out and submit a bug report.
 */
JS_STATIC_ASSERT(offsetof(JSGCArenaInfo, u) == 3 * sizeof(jsuword));

/*
 * Macros to convert between JSGCArenaInfo, the start address of the arena and
 * arena's page defined as (start address) >> GC_ARENA_SHIFT.
 */
#define ARENA_INFO_OFFSET (GC_ARENA_SIZE - (uint32) sizeof(JSGCArenaInfo))

#define IS_ARENA_INFO_ADDRESS(arena)                                          \
    (((jsuword) (arena) & GC_ARENA_MASK) == ARENA_INFO_OFFSET)

#define ARENA_START_TO_INFO(arenaStart)                                       \
    (JS_ASSERT(((arenaStart) & (jsuword) GC_ARENA_MASK) == 0),                \
     (JSGCArenaInfo *) ((arenaStart) + (jsuword) ARENA_INFO_OFFSET))

#define ARENA_INFO_TO_START(arena)                                            \
    (JS_ASSERT(IS_ARENA_INFO_ADDRESS(arena)),                                 \
     (jsuword) (arena) & ~(jsuword) GC_ARENA_MASK)

#define ARENA_PAGE_TO_INFO(arenaPage)                                         \
    (JS_ASSERT(arenaPage != 0),                                               \
     JS_ASSERT(!((jsuword)(arenaPage) >> (JS_BITS_PER_WORD-GC_ARENA_SHIFT))), \
     ARENA_START_TO_INFO((arenaPage) << GC_ARENA_SHIFT))

#define ARENA_INFO_TO_PAGE(arena)                                             \
    (JS_ASSERT(IS_ARENA_INFO_ADDRESS(arena)),                                 \
     ((jsuword) (arena) >> GC_ARENA_SHIFT))

#define GET_ARENA_INFO(chunk, index)                                          \
    (JS_ASSERT((index) < js_gcArenasPerChunk),                                \
     ARENA_START_TO_INFO(chunk + ((index) << GC_ARENA_SHIFT)))

#if CHUNKED_ARENA_ALLOCATION
/*
 * Definitions for allocating arenas in chunks.
 *
 * All chunks that have at least one free arena are put on the doubly-linked
 * list with the head stored in JSRuntime.gcChunkList. JSGCChunkInfo contains
 * the head of the chunk's free arena list together with the link fields for
 * gcChunkList.
 *
 * Structure stored in one of chunk's free arenas. GET_CHUNK_INFO_INDEX gives
 * the index of this arena. When all arenas in the chunk are used, it is
 * removed from the list and the index is set to NO_FREE_ARENAS indicating
 * that the chunk is not on gcChunkList and has no JSGCChunkInfo available.
 */

struct JSGCChunkInfo {
    JSGCChunkInfo   **prevp;
    JSGCChunkInfo   *next;
    JSGCArenaInfo   *lastFreeArena;
    uint32          numFreeArenas;
};

#define NO_FREE_ARENAS              JS_BITMASK(GC_ARENA_SHIFT - 1)

#ifdef js_gcArenasPerChunk
JS_STATIC_ASSERT(1 <= js_gcArenasPerChunk &&
                 js_gcArenasPerChunk <= NO_FREE_ARENAS);
#endif

#define GET_ARENA_CHUNK(arena, index)                                         \
    (JS_ASSERT(GET_ARENA_INDEX(arena) == index),                              \
     ARENA_INFO_TO_START(arena) - ((index) << GC_ARENA_SHIFT))

#define GET_ARENA_INDEX(arena)                                                \
    ((arena)->firstArena ? 0 : (uint32) (arena)->arenaIndex)

#define GET_CHUNK_INFO_INDEX(chunk)                                           \
    ((uint32) ARENA_START_TO_INFO(chunk)->arenaIndex)

#define SET_CHUNK_INFO_INDEX(chunk, index)                                    \
    (JS_ASSERT((index) < js_gcArenasPerChunk || (index) == NO_FREE_ARENAS),   \
     (void) (ARENA_START_TO_INFO(chunk)->arenaIndex = (jsuword) (index)))

#define GET_CHUNK_INFO(chunk, infoIndex)                                      \
    (JS_ASSERT(GET_CHUNK_INFO_INDEX(chunk) == (infoIndex)),                   \
     JS_ASSERT((uint32) (infoIndex) < js_gcArenasPerChunk),                   \
     (JSGCChunkInfo *) ((chunk) + ((infoIndex) << GC_ARENA_SHIFT)))

#define CHUNK_INFO_TO_INDEX(ci)                                               \
    GET_ARENA_INDEX(ARENA_START_TO_INFO((jsuword)ci))

#endif

/*
 * Macros for GC-thing operations.
 */
#define THINGS_PER_ARENA(thingSize)                                           \
    ((GC_ARENA_SIZE - (uint32) sizeof(JSGCArenaInfo)) / ((thingSize) + 1U))

#define THING_TO_ARENA(thing)                                                 \
    (JS_ASSERT(!JSString::isStatic(thing)),                                   \
     (JSGCArenaInfo *)(((jsuword) (thing) | GC_ARENA_MASK)                    \
                       + 1 - sizeof(JSGCArenaInfo)))

#define THING_TO_INDEX(thing, thingSize)                                      \
    ((uint32) ((jsuword) (thing) & GC_ARENA_MASK) / (uint32) (thingSize))

#define THING_FLAGS_END(arena) ((uint8 *)(arena))

#define THING_FLAGP(arena, thingIndex)                                        \
    (JS_ASSERT((jsuword) (thingIndex)                                         \
               < (jsuword) THINGS_PER_ARENA((arena)->list->thingSize)),       \
     (uint8 *)(arena) - 1 - (thingIndex))

#define THING_TO_FLAGP(thing, thingSize)                                      \
    THING_FLAGP(THING_TO_ARENA(thing), THING_TO_INDEX(thing, thingSize))

#define FLAGP_TO_ARENA(flagp) THING_TO_ARENA(flagp)

#define FLAGP_TO_INDEX(flagp)                                                 \
    (JS_ASSERT(((jsuword) (flagp) & GC_ARENA_MASK) < ARENA_INFO_OFFSET),      \
     (ARENA_INFO_OFFSET - 1 - (uint32) ((jsuword) (flagp) & GC_ARENA_MASK)))

#define FLAGP_TO_THING(flagp, thingSize)                                      \
    (JS_ASSERT(((jsuword) (flagp) & GC_ARENA_MASK) >=                         \
               (ARENA_INFO_OFFSET - THINGS_PER_ARENA(thingSize))),            \
     (JSGCThing *)(((jsuword) (flagp) & ~GC_ARENA_MASK) +                     \
                   (thingSize) * FLAGP_TO_INDEX(flagp)))

/*
 * Macros for the specialized arena for doubles.
 *
 * DOUBLES_PER_ARENA defines the maximum number of doubles that the arena can
 * hold. We find it as the following. Let n be the number of doubles in the
 * arena. Together with the bitmap of flags and JSGCArenaInfo they should fit
 * the arena. Hence DOUBLES_PER_ARENA or n_max is the maximum value of n for
 * which the following holds:
 *
 *   n*s + ceil(n/B) <= M                                               (1)
 *
 * where "/" denotes normal real division,
 *       ceil(r) gives the least integer not smaller than the number r,
 *       s is the number of words in jsdouble,
 *       B is number of bits per word or B == JS_BITS_PER_WORD
 *       M is the number of words in the arena before JSGCArenaInfo or
 *       M == (GC_ARENA_SIZE - sizeof(JSGCArenaInfo)) / sizeof(jsuword).
 *       M == ARENA_INFO_OFFSET / sizeof(jsuword)
 *
 * We rewrite the inequality as
 *
 *   n*B*s/B + ceil(n/B) <= M,
 *   ceil(n*B*s/B + n/B) <= M,
 *   ceil(n*(B*s + 1)/B) <= M                                           (2)
 *
 * We define a helper function e(n, s, B),
 *
 *   e(n, s, B) := ceil(n*(B*s + 1)/B) - n*(B*s + 1)/B, 0 <= e(n, s, B) < 1.
 *
 * It gives:
 *
 *   n*(B*s + 1)/B + e(n, s, B) <= M,
 *   n + e*B/(B*s + 1) <= M*B/(B*s + 1)
 *
 * We apply the floor function to both sides of the last equation, where
 * floor(r) gives the biggest integer not greater than r. As a consequence we
 * have:
 *
 *   floor(n + e*B/(B*s + 1)) <= floor(M*B/(B*s + 1)),
 *   n + floor(e*B/(B*s + 1)) <= floor(M*B/(B*s + 1)),
 *   n <= floor(M*B/(B*s + 1)),                                         (3)
 *
 * where floor(e*B/(B*s + 1)) is zero as e*B/(B*s + 1) < B/(B*s + 1) < 1.
 * Thus any n that satisfies the original constraint (1) or its equivalent (2),
 * must also satisfy (3). That is, we got an upper estimate for the maximum
 * value of n. Lets show that this upper estimate,
 *
 *   floor(M*B/(B*s + 1)),                                              (4)
 *
 * also satisfies (1) and, as such, gives the required maximum value.
 * Substituting it into (2) gives:
 *
 *   ceil(floor(M*B/(B*s + 1))*(B*s + 1)/B) == ceil(floor(M/X)*X)
 *
 * where X == (B*s + 1)/B > 1. But then floor(M/X)*X <= M/X*X == M and
 *
 *   ceil(floor(M/X)*X) <= ceil(M) == M.
 *
 * Thus the value of (4) gives the maximum n satisfying (1).
 *
 * For the final result we observe that in (4)
 *
 *    M*B == ARENA_INFO_OFFSET / sizeof(jsuword) * JS_BITS_PER_WORD
 *        == ARENA_INFO_OFFSET * JS_BITS_PER_BYTE
 *
 *  and
 *
 *    B*s == JS_BITS_PER_WORD * sizeof(jsdouble) / sizeof(jsuword)
 *        == JS_BITS_PER_DOUBLE.
 */
const size_t DOUBLES_PER_ARENA =
    (ARENA_INFO_OFFSET * JS_BITS_PER_BYTE) / (JS_BITS_PER_DOUBLE + 1);

/*
 * Check that  ARENA_INFO_OFFSET and sizeof(jsdouble) divides sizeof(jsuword).
 */
JS_STATIC_ASSERT(ARENA_INFO_OFFSET % sizeof(jsuword) == 0);
JS_STATIC_ASSERT(sizeof(jsdouble) % sizeof(jsuword) == 0);
JS_STATIC_ASSERT(sizeof(jsbitmap) == sizeof(jsuword));

const size_t DOUBLES_ARENA_BITMAP_WORDS =
    JS_HOWMANY(DOUBLES_PER_ARENA, JS_BITS_PER_WORD);

/* Check that DOUBLES_PER_ARENA indeed maximises (1). */
JS_STATIC_ASSERT(DOUBLES_PER_ARENA * sizeof(jsdouble) +
                 DOUBLES_ARENA_BITMAP_WORDS * sizeof(jsuword) <=
                 ARENA_INFO_OFFSET);

JS_STATIC_ASSERT((DOUBLES_PER_ARENA + 1) * sizeof(jsdouble) +
                 sizeof(jsuword) *
                 JS_HOWMANY((DOUBLES_PER_ARENA + 1), JS_BITS_PER_WORD) >
                 ARENA_INFO_OFFSET);

/*
 * When DOUBLES_PER_ARENA % BITS_PER_DOUBLE_FLAG_UNIT != 0, some bits in the
 * last byte of the occupation bitmap are unused.
 */
const size_t UNUSED_DOUBLE_BITMAP_BITS =
    DOUBLES_ARENA_BITMAP_WORDS * JS_BITS_PER_WORD - DOUBLES_PER_ARENA;

JS_STATIC_ASSERT(UNUSED_DOUBLE_BITMAP_BITS < JS_BITS_PER_WORD);

const size_t DOUBLES_ARENA_BITMAP_OFFSET =
    ARENA_INFO_OFFSET - DOUBLES_ARENA_BITMAP_WORDS * sizeof(jsuword);

#define CHECK_DOUBLE_ARENA_INFO(arenaInfo)                                    \
    (JS_ASSERT(IS_ARENA_INFO_ADDRESS(arenaInfo)),                             \
     JS_ASSERT(!(arenaInfo)->list))                                           \

/*
 * Get the start of the bitmap area containing double mark flags in the arena.
 * To access the flag the code uses
 *
 *   JS_TEST_BIT(bitmapStart, index)
 *
 * That is, compared with the case of arenas with non-double things, we count
 * flags from the start of the bitmap area, not from the end.
 */
#define DOUBLE_ARENA_BITMAP(arenaInfo)                                        \
    (CHECK_DOUBLE_ARENA_INFO(arenaInfo),                                      \
     (jsbitmap *) arenaInfo - DOUBLES_ARENA_BITMAP_WORDS)

#define DOUBLE_ARENA_BITMAP_END(arenaInfo)                                    \
    (CHECK_DOUBLE_ARENA_INFO(arenaInfo), (jsbitmap *) (arenaInfo))

#define DOUBLE_THING_TO_INDEX(thing)                                          \
    (CHECK_DOUBLE_ARENA_INFO(THING_TO_ARENA(thing)),                          \
     JS_ASSERT(((jsuword) (thing) & GC_ARENA_MASK) <                          \
               DOUBLES_ARENA_BITMAP_OFFSET),                                  \
     ((uint32) (((jsuword) (thing) & GC_ARENA_MASK) / sizeof(jsdouble))))

static void
ClearDoubleArenaFlags(JSGCArenaInfo *a)
{
    /*
     * When some high bits in the last byte of the double occupation bitmap
     * are unused, we must set them. Otherwise TurnUsedArenaIntoDoubleList
     * will assume that they corresponds to some free cells and tries to
     * allocate them.
     *
     * Note that the code works correctly with UNUSED_DOUBLE_BITMAP_BITS == 0.
     */
    jsbitmap *bitmap = DOUBLE_ARENA_BITMAP(a);
    memset(bitmap, 0, (DOUBLES_ARENA_BITMAP_WORDS - 1) * sizeof *bitmap);
    jsbitmap mask = ((jsbitmap) 1 << UNUSED_DOUBLE_BITMAP_BITS) - 1;
    size_t nused = JS_BITS_PER_WORD - UNUSED_DOUBLE_BITMAP_BITS;
    bitmap[DOUBLES_ARENA_BITMAP_WORDS - 1] = mask << nused;
}

static JS_ALWAYS_INLINE JSBool
IsMarkedDouble(JSGCArenaInfo *a, uint32 index)
{
    jsbitmap *bitmap;

    JS_ASSERT(a->u.hasMarkedDoubles);
    bitmap = DOUBLE_ARENA_BITMAP(a);
    return JS_TEST_BIT(bitmap, index);
}

#ifdef JS_THREADSAFE
/*
 * The maximum number of things to put on the local free list by taking
 * several things from the global free list or from the tail of the last
 * allocated arena to amortize the cost of rt->gcLock.
 */
#define MAX_THREAD_LOCAL_THINGS 64

#endif

JS_STATIC_ASSERT(sizeof(JSStackHeader) >= 2 * sizeof(jsval));

JS_STATIC_ASSERT(sizeof(JSGCThing) >= sizeof(JSString));
JS_STATIC_ASSERT(sizeof(JSGCThing) >= sizeof(jsdouble));

/* We want to use all the available GC thing space for object's slots. */
JS_STATIC_ASSERT(sizeof(JSObject) % sizeof(JSGCThing) == 0);

/*
 * JSPtrTable capacity growth descriptor. The table grows by powers of two
 * starting from capacity JSPtrTableInfo.minCapacity, but switching to linear
 * growth when capacity reaches JSPtrTableInfo.linearGrowthThreshold.
 */
typedef struct JSPtrTableInfo {
    uint16      minCapacity;
    uint16      linearGrowthThreshold;
} JSPtrTableInfo;

#define GC_ITERATOR_TABLE_MIN     4
#define GC_ITERATOR_TABLE_LINEAR  1024

static const JSPtrTableInfo iteratorTableInfo = {
    GC_ITERATOR_TABLE_MIN,
    GC_ITERATOR_TABLE_LINEAR
};

/* Calculate table capacity based on the current value of JSPtrTable.count. */
static size_t
PtrTableCapacity(size_t count, const JSPtrTableInfo *info)
{
    size_t linear, log, capacity;

    linear = info->linearGrowthThreshold;
    JS_ASSERT(info->minCapacity <= linear);

    if (count == 0) {
        capacity = 0;
    } else if (count < linear) {
        log = JS_CEILING_LOG2W(count);
        JS_ASSERT(log != JS_BITS_PER_WORD);
        capacity = (size_t)1 << log;
        if (capacity < info->minCapacity)
            capacity = info->minCapacity;
    } else {
        capacity = JS_ROUNDUP(count, linear);
    }

    JS_ASSERT(capacity >= count);
    return capacity;
}

static void
FreePtrTable(JSPtrTable *table, const JSPtrTableInfo *info)
{
    if (table->array) {
        JS_ASSERT(table->count > 0);
        js_free(table->array);
        table->array = NULL;
        table->count = 0;
    }
    JS_ASSERT(table->count == 0);
}

static JSBool
AddToPtrTable(JSContext *cx, JSPtrTable *table, const JSPtrTableInfo *info,
              void *ptr)
{
    size_t count, capacity;
    void **array;

    count = table->count;
    capacity = PtrTableCapacity(count, info);

    if (count == capacity) {
        if (capacity < info->minCapacity) {
            JS_ASSERT(capacity == 0);
            JS_ASSERT(!table->array);
            capacity = info->minCapacity;
        } else {
            /*
             * Simplify the overflow detection assuming pointer is bigger
             * than byte.
             */
            JS_STATIC_ASSERT(2 <= sizeof table->array[0]);
            capacity = (capacity < info->linearGrowthThreshold)
                       ? 2 * capacity
                       : capacity + info->linearGrowthThreshold;
            if (capacity > (size_t)-1 / sizeof table->array[0])
                goto bad;
        }
        array = (void **) js_realloc(table->array,
                                     capacity * sizeof table->array[0]);
        if (!array)
            goto bad;
#ifdef DEBUG
        memset(array + count, JS_FREE_PATTERN,
               (capacity - count) * sizeof table->array[0]);
#endif
        table->array = array;
    }

    table->array[count] = ptr;
    table->count = count + 1;

    return JS_TRUE;

  bad:
    JS_ReportOutOfMemory(cx);
    return JS_FALSE;
}

static void
ShrinkPtrTable(JSPtrTable *table, const JSPtrTableInfo *info,
               size_t newCount)
{
    size_t oldCapacity, capacity;
    void **array;

    JS_ASSERT(newCount <= table->count);
    if (newCount == table->count)
        return;

    oldCapacity = PtrTableCapacity(table->count, info);
    table->count = newCount;
    capacity = PtrTableCapacity(newCount, info);

    if (oldCapacity != capacity) {
        array = table->array;
        JS_ASSERT(array);
        if (capacity == 0) {
            js_free(array);
            table->array = NULL;
            return;
        }
        array = (void **) js_realloc(array, capacity * sizeof array[0]);
        if (array)
            table->array = array;
    }
#ifdef DEBUG
    memset(table->array + newCount, JS_FREE_PATTERN,
           (capacity - newCount) * sizeof table->array[0]);
#endif
}

#ifdef JS_GCMETER
# define METER(x)               ((void) (x))
# define METER_IF(condition, x) ((void) ((condition) && (x)))
#else
# define METER(x)               ((void) 0)
# define METER_IF(condition, x) ((void) 0)
#endif

#define METER_UPDATE_MAX(maxLval, rval)                                       \
    METER_IF((maxLval) < (rval), (maxLval) = (rval))

#if JS_GC_USE_MMAP || !HAS_POSIX_MEMALIGN

/*
 * For chunks allocated via over-sized malloc, get a pointer to store the gap
 * between the malloc's result and the first arena in the chunk.
 */
static uint32 *
GetMallocedChunkGapPtr(jsuword chunk)
{
    JS_ASSERT((chunk & GC_ARENA_MASK) == 0);

    /* Use the memory after the chunk, see NewGCChunk for details. */
    return (uint32 *) (chunk + (js_gcArenasPerChunk << GC_ARENA_SHIFT));
}

#endif

static jsuword
NewGCChunk(void)
{
    void *p;

#if JS_GC_USE_MMAP
    if (js_gcUseMmap) {
# if defined(XP_WIN)
        p = VirtualAlloc(NULL, js_gcArenasPerChunk << GC_ARENA_SHIFT,
                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        return (jsuword) p;
# else
        p = mmap(NULL, js_gcArenasPerChunk << GC_ARENA_SHIFT,
                 PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        return (p == MAP_FAILED) ? 0 : (jsuword) p;
# endif
    }
#endif

#if HAS_POSIX_MEMALIGN
    if (0 != posix_memalign(&p, GC_ARENA_SIZE,
                            GC_ARENA_SIZE * js_gcArenasPerChunk -
                            JS_GC_ARENA_PAD)) {
        return 0;
    }
    return (jsuword) p;
#else
    /*
     * Implement chunk allocation using oversized malloc if mmap and
     * posix_memalign are not available.
     *
     * Since malloc allocates pointers aligned on the word boundary, to get
     * js_gcArenasPerChunk aligned arenas, we need to malloc only
     *
     *   ((js_gcArenasPerChunk + 1) << GC_ARENA_SHIFT) - sizeof(size_t)
     *
     * bytes. But since we stores the gap between the malloced pointer and the
     * first arena in the chunk after the chunk, we need to ask for
     *
     *   ((js_gcArenasPerChunk + 1) << GC_ARENA_SHIFT)
     *
     * bytes to ensure that we always have room to store the gap.
     */
    p = js_malloc((js_gcArenasPerChunk + 1) << GC_ARENA_SHIFT);
    if (!p)
        return 0;

    {
        jsuword chunk;

        chunk = ((jsuword) p + GC_ARENA_MASK) & ~GC_ARENA_MASK;
        *GetMallocedChunkGapPtr(chunk) = (uint32) (chunk - (jsuword) p);
        return chunk;
    }
#endif
}

static void
DestroyGCChunk(jsuword chunk)
{
    JS_ASSERT((chunk & GC_ARENA_MASK) == 0);
#if JS_GC_USE_MMAP
    if (js_gcUseMmap) {
# if defined(XP_WIN)
        VirtualFree((void *) chunk, 0, MEM_RELEASE);
# elif defined(SOLARIS)
        munmap((char *) chunk, js_gcArenasPerChunk << GC_ARENA_SHIFT);
# else
        munmap((void *) chunk, js_gcArenasPerChunk << GC_ARENA_SHIFT);
# endif
        return;
    }
#endif

#if HAS_POSIX_MEMALIGN
    js_free((void *) chunk);
#else
    /* See comments in NewGCChunk. */
    JS_ASSERT(*GetMallocedChunkGapPtr(chunk) < GC_ARENA_SIZE);
    js_free((void *) (chunk - *GetMallocedChunkGapPtr(chunk)));
#endif
}

#if CHUNKED_ARENA_ALLOCATION

static void
AddChunkToList(JSRuntime *rt, JSGCChunkInfo *ci)
{
    ci->prevp = &rt->gcChunkList;
    ci->next = rt->gcChunkList;
    if (rt->gcChunkList) {
        JS_ASSERT(rt->gcChunkList->prevp == &rt->gcChunkList);
        rt->gcChunkList->prevp = &ci->next;
    }
    rt->gcChunkList = ci;
}

static void
RemoveChunkFromList(JSRuntime *rt, JSGCChunkInfo *ci)
{
    *ci->prevp = ci->next;
    if (ci->next) {
        JS_ASSERT(ci->next->prevp == &ci->next);
        ci->next->prevp = ci->prevp;
    }
}

#endif

static JSGCArenaInfo *
NewGCArena(JSRuntime *rt)
{
    jsuword chunk;
    JSGCArenaInfo *a;

    if (rt->gcBytes >= rt->gcMaxBytes)
        return NULL;

#if CHUNKED_ARENA_ALLOCATION
    if (js_gcArenasPerChunk == 1) {
#endif
        chunk = NewGCChunk();
        if (chunk == 0)
            return NULL;
        a = ARENA_START_TO_INFO(chunk);
#if CHUNKED_ARENA_ALLOCATION
    } else {
        JSGCChunkInfo *ci;
        uint32 i;
        JSGCArenaInfo *aprev;

        ci = rt->gcChunkList;
        if (!ci) {
            chunk = NewGCChunk();
            if (chunk == 0)
                return NULL;
            JS_ASSERT((chunk & GC_ARENA_MASK) == 0);
            a = GET_ARENA_INFO(chunk, 0);
            a->firstArena = JS_TRUE;
            a->arenaIndex = 0;
            aprev = NULL;
            i = 0;
            do {
                a->prev = aprev;
                aprev = a;
                ++i;
                a = GET_ARENA_INFO(chunk, i);
                a->firstArena = JS_FALSE;
                a->arenaIndex = i;
            } while (i != js_gcArenasPerChunk - 1);
            ci = GET_CHUNK_INFO(chunk, 0);
            ci->lastFreeArena = aprev;
            ci->numFreeArenas = js_gcArenasPerChunk - 1;
            AddChunkToList(rt, ci);
        } else {
            JS_ASSERT(ci->prevp == &rt->gcChunkList);
            a = ci->lastFreeArena;
            aprev = a->prev;
            if (!aprev) {
                JS_ASSERT(ci->numFreeArenas == 1);
                JS_ASSERT(ARENA_INFO_TO_START(a) == (jsuword) ci);
                RemoveChunkFromList(rt, ci);
                chunk = GET_ARENA_CHUNK(a, GET_ARENA_INDEX(a));
                SET_CHUNK_INFO_INDEX(chunk, NO_FREE_ARENAS);
            } else {
                JS_ASSERT(ci->numFreeArenas >= 2);
                JS_ASSERT(ARENA_INFO_TO_START(a) != (jsuword) ci);
                ci->lastFreeArena = aprev;
                ci->numFreeArenas--;
            }
        }
    }
#endif

    rt->gcBytes += GC_ARENA_SIZE;
    a->prevUntracedPage = 0;
    memset(&a->u, 0, sizeof(a->u));

    return a;
}

static void
DestroyGCArenas(JSRuntime *rt, JSGCArenaInfo *last)
{
    JSGCArenaInfo *a;

    while (last) {
        a = last;
        last = last->prev;

        METER(rt->gcStats.afree++);
        JS_ASSERT(rt->gcBytes >= GC_ARENA_SIZE);
        rt->gcBytes -= GC_ARENA_SIZE;

#if CHUNKED_ARENA_ALLOCATION
        if (js_gcArenasPerChunk == 1) {
#endif
            DestroyGCChunk(ARENA_INFO_TO_START(a));
#if CHUNKED_ARENA_ALLOCATION
        } else {
            uint32 arenaIndex;
            jsuword chunk;
            uint32 chunkInfoIndex;
            JSGCChunkInfo *ci;
# ifdef DEBUG
            jsuword firstArena;

            firstArena = a->firstArena;
            arenaIndex = a->arenaIndex;
            memset((void *) ARENA_INFO_TO_START(a), JS_FREE_PATTERN,
                   GC_ARENA_SIZE - JS_GC_ARENA_PAD);
            a->firstArena = firstArena;
            a->arenaIndex = arenaIndex;
# endif
            arenaIndex = GET_ARENA_INDEX(a);
            chunk = GET_ARENA_CHUNK(a, arenaIndex);
            chunkInfoIndex = GET_CHUNK_INFO_INDEX(chunk);
            if (chunkInfoIndex == NO_FREE_ARENAS) {
                chunkInfoIndex = arenaIndex;
                SET_CHUNK_INFO_INDEX(chunk, arenaIndex);
                ci = GET_CHUNK_INFO(chunk, chunkInfoIndex);
                a->prev = NULL;
                ci->lastFreeArena = a;
                ci->numFreeArenas = 1;
                AddChunkToList(rt, ci);
            } else {
                JS_ASSERT(chunkInfoIndex != arenaIndex);
                ci = GET_CHUNK_INFO(chunk, chunkInfoIndex);
                JS_ASSERT(ci->numFreeArenas != 0);
                JS_ASSERT(ci->lastFreeArena);
                JS_ASSERT(a != ci->lastFreeArena);
                if (ci->numFreeArenas == js_gcArenasPerChunk - 1) {
                    RemoveChunkFromList(rt, ci);
                    DestroyGCChunk(chunk);
                } else {
                    ++ci->numFreeArenas;
                    a->prev = ci->lastFreeArena;
                    ci->lastFreeArena = a;
                }
            }
        }
# endif
    }
}

static inline size_t
GetFinalizableThingSize(unsigned thingKind)
{
    JS_STATIC_ASSERT(JS_EXTERNAL_STRING_LIMIT == 8);

    static const uint8 map[FINALIZE_LIMIT] = {
        sizeof(JSObject),   /* FINALIZE_OBJECT */
        sizeof(JSFunction), /* FINALIZE_FUNCTION */
#if JS_HAS_XML_SUPPORT
        sizeof(JSXML),      /* FINALIZE_XML */
#endif
        sizeof(JSString),   /* FINALIZE_STRING */
        sizeof(JSString),   /* FINALIZE_EXTERNAL_STRING0 */
        sizeof(JSString),   /* FINALIZE_EXTERNAL_STRING1 */
        sizeof(JSString),   /* FINALIZE_EXTERNAL_STRING2 */
        sizeof(JSString),   /* FINALIZE_EXTERNAL_STRING3 */
        sizeof(JSString),   /* FINALIZE_EXTERNAL_STRING4 */
        sizeof(JSString),   /* FINALIZE_EXTERNAL_STRING5 */
        sizeof(JSString),   /* FINALIZE_EXTERNAL_STRING6 */
        sizeof(JSString),   /* FINALIZE_EXTERNAL_STRING7 */
    };

    JS_ASSERT(thingKind < FINALIZE_LIMIT);
    return map[thingKind];
}

static inline size_t
GetFinalizableArenaTraceKind(JSGCArenaInfo *a)
{
    JS_STATIC_ASSERT(JS_EXTERNAL_STRING_LIMIT == 8);
    JS_ASSERT(a->list);

    static const uint8 map[FINALIZE_LIMIT] = {
        JSTRACE_OBJECT,     /* FINALIZE_OBJECT */
        JSTRACE_OBJECT,     /* FINALIZE_FUNCTION */
#if JS_HAS_XML_SUPPORT      /* FINALIZE_XML */
        JSTRACE_XML,
#endif                      /* FINALIZE_STRING */
        JSTRACE_STRING,
        JSTRACE_STRING,     /* FINALIZE_EXTERNAL_STRING0 */
        JSTRACE_STRING,     /* FINALIZE_EXTERNAL_STRING1 */
        JSTRACE_STRING,     /* FINALIZE_EXTERNAL_STRING2 */
        JSTRACE_STRING,     /* FINALIZE_EXTERNAL_STRING3 */
        JSTRACE_STRING,     /* FINALIZE_EXTERNAL_STRING4 */
        JSTRACE_STRING,     /* FINALIZE_EXTERNAL_STRING5 */
        JSTRACE_STRING,     /* FINALIZE_EXTERNAL_STRING6 */
        JSTRACE_STRING,     /* FINALIZE_EXTERNAL_STRING7 */
    };

    JS_ASSERT(a->list->thingKind < FINALIZE_LIMIT);
    return map[a->list->thingKind];
}

static void
InitGCArenaLists(JSRuntime *rt)
{
    for (unsigned i = 0; i != FINALIZE_LIMIT; ++i) {
        JSGCArenaList *arenaList = &rt->gcArenaList[i];
        arenaList->thingSize = GetFinalizableThingSize(i);
        arenaList->last = NULL;
        arenaList->lastCount = THINGS_PER_ARENA(arenaList->thingSize);
        arenaList->thingKind = i;
        arenaList->freeList = NULL;
    }
    rt->gcDoubleArenaList.first = NULL;
    rt->gcDoubleArenaList.cursor = NULL;
}

static void
FinishGCArenaLists(JSRuntime *rt)
{
    for (unsigned i = 0; i < FINALIZE_LIMIT; i++) {
        JSGCArenaList *arenaList = &rt->gcArenaList[i];
        DestroyGCArenas(rt, arenaList->last);
        arenaList->last = NULL;
        arenaList->lastCount = THINGS_PER_ARENA(arenaList->thingSize);
        arenaList->freeList = NULL;
    }
    DestroyGCArenas(rt, rt->gcDoubleArenaList.first);
    rt->gcDoubleArenaList.first = NULL;
    rt->gcDoubleArenaList.cursor = NULL;

    rt->gcBytes = 0;
    JS_ASSERT(rt->gcChunkList == 0);
}

/*
 * This function must not be called when thing is jsdouble.
 */
static uint8 *
GetGCThingFlags(void *thing)
{
    JSGCArenaInfo *a;
    uint32 index;

    a = THING_TO_ARENA(thing);
    index = THING_TO_INDEX(thing, a->list->thingSize);
    return THING_FLAGP(a, index);
}

intN
js_GetExternalStringGCType(JSString *str)
{
    JS_STATIC_ASSERT(FINALIZE_STRING + 1 == FINALIZE_EXTERNAL_STRING0);
    JS_ASSERT(!JSString::isStatic(str));

    unsigned thingKind = THING_TO_ARENA(str)->list->thingKind;
    JS_ASSERT(IsFinalizableStringKind(thingKind));
    return intN(thingKind) - intN(FINALIZE_EXTERNAL_STRING0);
}

JS_FRIEND_API(uint32)
js_GetGCThingTraceKind(void *thing)
{
    if (JSString::isStatic(thing))
        return JSTRACE_STRING;

    JSGCArenaInfo *a = THING_TO_ARENA(thing);
    if (!a->list)
        return JSTRACE_DOUBLE;
    return GetFinalizableArenaTraceKind(a);
}

JSRuntime*
js_GetGCStringRuntime(JSString *str)
{
    JSGCArenaList *list = THING_TO_ARENA(str)->list;
    JS_ASSERT(list->thingSize == sizeof(JSString));

    unsigned i = list->thingKind;
    JS_ASSERT(i == FINALIZE_STRING ||
              (FINALIZE_EXTERNAL_STRING0 <= i &&
               i < FINALIZE_EXTERNAL_STRING0 + JS_EXTERNAL_STRING_LIMIT));
    return (JSRuntime *)((uint8 *)(list - i) -
                         offsetof(JSRuntime, gcArenaList));
}

JSBool
js_IsAboutToBeFinalized(JSContext *cx, void *thing)
{
    JSGCArenaInfo *a;
    uint32 index, flags;

    a = THING_TO_ARENA(thing);
    if (!a->list) {
        /*
         * Check if arena has no marked doubles. In that case the bitmap with
         * the mark flags contains all garbage as it is initialized only when
         * marking the first double in the arena.
         */
        if (!a->u.hasMarkedDoubles)
            return JS_TRUE;
        index = DOUBLE_THING_TO_INDEX(thing);
        return !IsMarkedDouble(a, index);
    }
    index = THING_TO_INDEX(thing, a->list->thingSize);
    flags = *THING_FLAGP(a, index);
    return !(flags & (GCF_MARK | GCF_LOCK | GCF_FINAL));
}

/* This is compatible with JSDHashEntryStub. */
typedef struct JSGCRootHashEntry {
    JSDHashEntryHdr hdr;
    void            *root;
    const char      *name;
} JSGCRootHashEntry;

/* Initial size of the gcRootsHash table (SWAG, small enough to amortize). */
#define GC_ROOTS_SIZE   256

#if CHUNKED_ARENA_ALLOCATION

/*
 * For a CPU with extremely large pages using them for GC things wastes
 * too much memory.
 */
# define GC_ARENAS_PER_CPU_PAGE_LIMIT JS_BIT(18 - GC_ARENA_SHIFT)

JS_STATIC_ASSERT(GC_ARENAS_PER_CPU_PAGE_LIMIT <= NO_FREE_ARENAS);

#endif

JSBool
js_InitGC(JSRuntime *rt, uint32 maxbytes)
{
#if JS_GC_USE_MMAP
    if (js_gcArenasPerChunk == 0) {
        size_t cpuPageSize, arenasPerPage;
# if defined(XP_WIN)
        SYSTEM_INFO si;

        GetSystemInfo(&si);
        cpuPageSize = si.dwPageSize;

# elif defined(XP_UNIX) || defined(XP_BEOS)
        cpuPageSize = (size_t) sysconf(_SC_PAGESIZE);
# else
#  error "Not implemented"
# endif
        /* cpuPageSize is a power of 2. */
        JS_ASSERT((cpuPageSize & (cpuPageSize - 1)) == 0);
        arenasPerPage = cpuPageSize >> GC_ARENA_SHIFT;
#ifdef DEBUG
        if (arenasPerPage == 0) {
            fprintf(stderr,
"JS engine warning: the size of the CPU page, %u bytes, is too low to use\n"
"paged allocation for the garbage collector. Please report this.\n",
                    (unsigned) cpuPageSize);
        }
#endif
        if (arenasPerPage - 1 <= (size_t) (GC_ARENAS_PER_CPU_PAGE_LIMIT - 1)) {
            /*
             * Use at least 4 GC arenas per paged allocation chunk to minimize
             * the overhead of mmap/VirtualAlloc.
             */
            js_gcUseMmap = JS_TRUE;
            js_gcArenasPerChunk = JS_MAX((uint32) arenasPerPage, 4);
        } else {
            js_gcUseMmap = JS_FALSE;
            js_gcArenasPerChunk = 7;
        }
    }
    JS_ASSERT(1 <= js_gcArenasPerChunk &&
              js_gcArenasPerChunk <= NO_FREE_ARENAS);
#endif

    InitGCArenaLists(rt);
    if (!JS_DHashTableInit(&rt->gcRootsHash, JS_DHashGetStubOps(), NULL,
                           sizeof(JSGCRootHashEntry), GC_ROOTS_SIZE)) {
        rt->gcRootsHash.ops = NULL;
        return JS_FALSE;
    }
    rt->gcLocksHash = NULL;     /* create lazily */

    /*
     * Separate gcMaxMallocBytes from gcMaxBytes but initialize to maxbytes
     * for default backward API compatibility.
     */
    rt->gcMaxBytes = rt->gcMaxMallocBytes = maxbytes;
    rt->gcEmptyArenaPoolLifespan = 30000;

    /*
     * By default the trigger factor gets maximum possible value. This
     * means that GC will not be triggered by growth of GC memory (gcBytes).
     */
    rt->setGCTriggerFactor((uint32) -1);

    /*
     * The assigned value prevents GC from running when GC memory is too low
     * (during JS engine start).
     */
    rt->setGCLastBytes(8192);

    METER(memset(&rt->gcStats, 0, sizeof rt->gcStats));
    return JS_TRUE;
}

#ifdef JS_GCMETER

static void
UpdateArenaStats(JSGCArenaStats *st, uint32 nlivearenas, uint32 nkilledArenas,
                 uint32 nthings)
{
    size_t narenas;

    narenas = nlivearenas + nkilledArenas;
    JS_ASSERT(narenas >= st->livearenas);

    st->newarenas = narenas - st->livearenas;
    st->narenas = narenas;
    st->livearenas = nlivearenas;
    if (st->maxarenas < narenas)
        st->maxarenas = narenas;
    st->totalarenas += narenas;

    st->nthings = nthings;
    if (st->maxthings < nthings)
        st->maxthings = nthings;
    st->totalthings += nthings;
}

JS_FRIEND_API(void)
js_DumpGCStats(JSRuntime *rt, FILE *fp)
{
    int i;
    size_t sumArenas, sumTotalArenas;
    size_t sumThings, sumMaxThings;
    size_t sumThingSize, sumTotalThingSize;
    size_t sumArenaCapacity, sumTotalArenaCapacity;
    JSGCArenaStats *st;
    size_t thingSize, thingsPerArena;
    size_t sumAlloc, sumLocalAlloc, sumFail, sumRetry;

    fprintf(fp, "\nGC allocation statistics:\n");

#define UL(x)       ((unsigned long)(x))
#define ULSTAT(x)   UL(rt->gcStats.x)
#define PERCENT(x,y)  (100.0 * (double) (x) / (double) (y))

    sumArenas = 0;
    sumTotalArenas = 0;
    sumThings = 0;
    sumMaxThings = 0;
    sumThingSize = 0;
    sumTotalThingSize = 0;
    sumArenaCapacity = 0;
    sumTotalArenaCapacity = 0;
    sumAlloc = 0;
    sumLocalAlloc = 0;
    sumFail = 0;
    sumRetry = 0;
    for (i = -1; i < (int) GC_NUM_FREELISTS; i++) {
        if (i == -1) {
            thingSize = sizeof(jsdouble);
            thingsPerArena = DOUBLES_PER_ARENA;
            st = &rt->gcStats.doubleArenaStats;
            fprintf(fp,
                    "Arena list for double values (%lu doubles per arena):",
                    UL(thingsPerArena));
        } else {
            thingSize = rt->gcArenaList[i].thingSize;
            thingsPerArena = THINGS_PER_ARENA(thingSize);
            st = &rt->gcStats.arenaStats[i];
            fprintf(fp,
                    "Arena list %d (thing size %lu, %lu things per arena):",
                    i, UL(GC_FREELIST_NBYTES(i)), UL(thingsPerArena));
        }
        if (st->maxarenas == 0) {
            fputs(" NEVER USED\n", fp);
            continue;
        }
        putc('\n', fp);
        fprintf(fp, "           arenas before GC: %lu\n", UL(st->narenas));
        fprintf(fp, "       new arenas before GC: %lu (%.1f%%)\n",
                UL(st->newarenas), PERCENT(st->newarenas, st->narenas));
        fprintf(fp, "            arenas after GC: %lu (%.1f%%)\n",
                UL(st->livearenas), PERCENT(st->livearenas, st->narenas));
        fprintf(fp, "                 max arenas: %lu\n", UL(st->maxarenas));
        fprintf(fp, "                     things: %lu\n", UL(st->nthings));
        fprintf(fp, "        GC cell utilization: %.1f%%\n",
                PERCENT(st->nthings, thingsPerArena * st->narenas));
        fprintf(fp, "   average cell utilization: %.1f%%\n",
                PERCENT(st->totalthings, thingsPerArena * st->totalarenas));
        fprintf(fp, "                 max things: %lu\n", UL(st->maxthings));
        fprintf(fp, "             alloc attempts: %lu\n", UL(st->alloc));
        fprintf(fp, "        alloc without locks: %1u  (%.1f%%)\n",
                UL(st->localalloc), PERCENT(st->localalloc, st->alloc));
        sumArenas += st->narenas;
        sumTotalArenas += st->totalarenas;
        sumThings += st->nthings;
        sumMaxThings += st->maxthings;
        sumThingSize += thingSize * st->nthings;
        sumTotalThingSize += thingSize * st->totalthings;
        sumArenaCapacity += thingSize * thingsPerArena * st->narenas;
        sumTotalArenaCapacity += thingSize * thingsPerArena * st->totalarenas;
        sumAlloc += st->alloc;
        sumLocalAlloc += st->localalloc;
        sumFail += st->fail;
        sumRetry += st->retry;
    }
    fprintf(fp, "TOTAL STATS:\n");
    fprintf(fp, "            bytes allocated: %lu\n", UL(rt->gcBytes));
    fprintf(fp, "            total GC arenas: %lu\n", UL(sumArenas));
    fprintf(fp, "            total GC things: %lu\n", UL(sumThings));
    fprintf(fp, "        max total GC things: %lu\n", UL(sumMaxThings));
    fprintf(fp, "        GC cell utilization: %.1f%%\n",
            PERCENT(sumThingSize, sumArenaCapacity));
    fprintf(fp, "   average cell utilization: %.1f%%\n",
            PERCENT(sumTotalThingSize, sumTotalArenaCapacity));
    fprintf(fp, "allocation retries after GC: %lu\n", UL(sumRetry));
    fprintf(fp, "             alloc attempts: %lu\n", UL(sumAlloc));
    fprintf(fp, "        alloc without locks: %1u  (%.1f%%)\n",
            UL(sumLocalAlloc), PERCENT(sumLocalAlloc, sumAlloc));
    fprintf(fp, "        allocation failures: %lu\n", UL(sumFail));
    fprintf(fp, "         things born locked: %lu\n", ULSTAT(lockborn));
    fprintf(fp, "           valid lock calls: %lu\n", ULSTAT(lock));
    fprintf(fp, "         valid unlock calls: %lu\n", ULSTAT(unlock));
    fprintf(fp, "       mark recursion depth: %lu\n", ULSTAT(depth));
    fprintf(fp, "     maximum mark recursion: %lu\n", ULSTAT(maxdepth));
    fprintf(fp, "     mark C recursion depth: %lu\n", ULSTAT(cdepth));
    fprintf(fp, "   maximum mark C recursion: %lu\n", ULSTAT(maxcdepth));
    fprintf(fp, "      delayed tracing calls: %lu\n", ULSTAT(untraced));
#ifdef DEBUG
    fprintf(fp, "      max trace later count: %lu\n", ULSTAT(maxuntraced));
#endif
    fprintf(fp, "   maximum GC nesting level: %lu\n", ULSTAT(maxlevel));
    fprintf(fp, "potentially useful GC calls: %lu\n", ULSTAT(poke));
    fprintf(fp, "  thing arenas freed so far: %lu\n", ULSTAT(afree));
    fprintf(fp, "     stack segments scanned: %lu\n", ULSTAT(stackseg));
    fprintf(fp, "stack segment slots scanned: %lu\n", ULSTAT(segslots));
    fprintf(fp, "reachable closeable objects: %lu\n", ULSTAT(nclose));
    fprintf(fp, "    max reachable closeable: %lu\n", ULSTAT(maxnclose));
    fprintf(fp, "      scheduled close hooks: %lu\n", ULSTAT(closelater));
    fprintf(fp, "  max scheduled close hooks: %lu\n", ULSTAT(maxcloselater));

#undef UL
#undef ULSTAT
#undef PERCENT

#ifdef JS_ARENAMETER
    JS_DumpArenaStats(fp);
#endif
}
#endif

#ifdef DEBUG
static void
CheckLeakedRoots(JSRuntime *rt);
#endif

void
js_FinishGC(JSRuntime *rt)
{
#ifdef JS_ARENAMETER
    JS_DumpArenaStats(stdout);
#endif
#ifdef JS_GCMETER
    js_DumpGCStats(rt, stdout);
#endif

    FreePtrTable(&rt->gcIteratorTable, &iteratorTableInfo);
    FinishGCArenaLists(rt);

    if (rt->gcRootsHash.ops) {
#ifdef DEBUG
        CheckLeakedRoots(rt);
#endif
        JS_DHashTableFinish(&rt->gcRootsHash);
        rt->gcRootsHash.ops = NULL;
    }
    if (rt->gcLocksHash) {
        JS_DHashTableDestroy(rt->gcLocksHash);
        rt->gcLocksHash = NULL;
    }
}

JSBool
js_AddRoot(JSContext *cx, void *rp, const char *name)
{
    JSBool ok = js_AddRootRT(cx->runtime, rp, name);
    if (!ok)
        JS_ReportOutOfMemory(cx);
    return ok;
}

JSBool
js_AddRootRT(JSRuntime *rt, void *rp, const char *name)
{
    JSBool ok;
    JSGCRootHashEntry *rhe;

    /*
     * Due to the long-standing, but now removed, use of rt->gcLock across the
     * bulk of js_GC, API users have come to depend on JS_AddRoot etc. locking
     * properly with a racing GC, without calling JS_AddRoot from a request.
     * We have to preserve API compatibility here, now that we avoid holding
     * rt->gcLock across the mark phase (including the root hashtable mark).
     */
    JS_LOCK_GC(rt);
    js_WaitForGC(rt);
    rhe = (JSGCRootHashEntry *)
          JS_DHashTableOperate(&rt->gcRootsHash, rp, JS_DHASH_ADD);
    if (rhe) {
        rhe->root = rp;
        rhe->name = name;
        ok = JS_TRUE;
    } else {
        ok = JS_FALSE;
    }
    JS_UNLOCK_GC(rt);
    return ok;
}

JSBool
js_RemoveRoot(JSRuntime *rt, void *rp)
{
    /*
     * Due to the JS_RemoveRootRT API, we may be called outside of a request.
     * Same synchronization drill as above in js_AddRoot.
     */
    JS_LOCK_GC(rt);
    js_WaitForGC(rt);
    (void) JS_DHashTableOperate(&rt->gcRootsHash, rp, JS_DHASH_REMOVE);
    rt->gcPoke = JS_TRUE;
    JS_UNLOCK_GC(rt);
    return JS_TRUE;
}

#ifdef DEBUG

static JSDHashOperator
js_root_printer(JSDHashTable *table, JSDHashEntryHdr *hdr, uint32 i, void *arg)
{
    uint32 *leakedroots = (uint32 *)arg;
    JSGCRootHashEntry *rhe = (JSGCRootHashEntry *)hdr;

    (*leakedroots)++;
    fprintf(stderr,
            "JS engine warning: leaking GC root \'%s\' at %p\n",
            rhe->name ? (char *)rhe->name : "", rhe->root);

    return JS_DHASH_NEXT;
}

static void
CheckLeakedRoots(JSRuntime *rt)
{
    uint32 leakedroots = 0;

    /* Warn (but don't assert) debug builds of any remaining roots. */
    JS_DHashTableEnumerate(&rt->gcRootsHash, js_root_printer,
                           &leakedroots);
    if (leakedroots > 0) {
        if (leakedroots == 1) {
            fprintf(stderr,
"JS engine warning: 1 GC root remains after destroying the JSRuntime at %p.\n"
"                   This root may point to freed memory. Objects reachable\n"
"                   through it have not been finalized.\n",
                    (void *) rt);
        } else {
            fprintf(stderr,
"JS engine warning: %lu GC roots remain after destroying the JSRuntime at %p.\n"
"                   These roots may point to freed memory. Objects reachable\n"
"                   through them have not been finalized.\n",
                    (unsigned long) leakedroots, (void *) rt);
        }
    }
}

typedef struct NamedRootDumpArgs {
    void (*dump)(const char *name, void *rp, void *data);
    void *data;
} NamedRootDumpArgs;

static JSDHashOperator
js_named_root_dumper(JSDHashTable *table, JSDHashEntryHdr *hdr, uint32 number,
                     void *arg)
{
    NamedRootDumpArgs *args = (NamedRootDumpArgs *) arg;
    JSGCRootHashEntry *rhe = (JSGCRootHashEntry *)hdr;

    if (rhe->name)
        args->dump(rhe->name, rhe->root, args->data);
    return JS_DHASH_NEXT;
}

JS_BEGIN_EXTERN_C
void
js_DumpNamedRoots(JSRuntime *rt,
                  void (*dump)(const char *name, void *rp, void *data),
                  void *data)
{
    NamedRootDumpArgs args;

    args.dump = dump;
    args.data = data;
    JS_DHashTableEnumerate(&rt->gcRootsHash, js_named_root_dumper, &args);
}
JS_END_EXTERN_C

#endif /* DEBUG */

typedef struct GCRootMapArgs {
    JSGCRootMapFun map;
    void *data;
} GCRootMapArgs;

static JSDHashOperator
js_gcroot_mapper(JSDHashTable *table, JSDHashEntryHdr *hdr, uint32 number,
                 void *arg)
{
    GCRootMapArgs *args = (GCRootMapArgs *) arg;
    JSGCRootHashEntry *rhe = (JSGCRootHashEntry *)hdr;
    intN mapflags;
    int op;

    mapflags = args->map(rhe->root, rhe->name, args->data);

#if JS_MAP_GCROOT_NEXT == JS_DHASH_NEXT &&                                     \
    JS_MAP_GCROOT_STOP == JS_DHASH_STOP &&                                     \
    JS_MAP_GCROOT_REMOVE == JS_DHASH_REMOVE
    op = (JSDHashOperator)mapflags;
#else
    op = JS_DHASH_NEXT;
    if (mapflags & JS_MAP_GCROOT_STOP)
        op |= JS_DHASH_STOP;
    if (mapflags & JS_MAP_GCROOT_REMOVE)
        op |= JS_DHASH_REMOVE;
#endif

    return (JSDHashOperator) op;
}

uint32
js_MapGCRoots(JSRuntime *rt, JSGCRootMapFun map, void *data)
{
    GCRootMapArgs args;
    uint32 rv;

    args.map = map;
    args.data = data;
    JS_LOCK_GC(rt);
    rv = JS_DHashTableEnumerate(&rt->gcRootsHash, js_gcroot_mapper, &args);
    JS_UNLOCK_GC(rt);
    return rv;
}

JSBool
js_RegisterCloseableIterator(JSContext *cx, JSObject *obj)
{
    JSRuntime *rt;
    JSBool ok;

    rt = cx->runtime;
    JS_ASSERT(!rt->gcRunning);

    JS_LOCK_GC(rt);
    ok = AddToPtrTable(cx, &rt->gcIteratorTable, &iteratorTableInfo, obj);
    JS_UNLOCK_GC(rt);
    return ok;
}

static void
CloseNativeIterators(JSContext *cx)
{
    JSRuntime *rt;
    size_t count, newCount, i;
    void **array;
    JSObject *obj;

    rt = cx->runtime;
    count = rt->gcIteratorTable.count;
    array = rt->gcIteratorTable.array;

    newCount = 0;
    for (i = 0; i != count; ++i) {
        obj = (JSObject *)array[i];
        if (js_IsAboutToBeFinalized(cx, obj))
            js_CloseNativeIterator(cx, obj);
        else
            array[newCount++] = obj;
    }
    ShrinkPtrTable(&rt->gcIteratorTable, &iteratorTableInfo, newCount);
}

#if defined(DEBUG_brendan) || defined(DEBUG_timeless)
#define DEBUG_gchist
#endif

#ifdef DEBUG_gchist
#define NGCHIST 64

static struct GCHist {
    bool        lastDitch;
    JSGCThing   *freeList;
} gchist[NGCHIST];

unsigned gchpos = 0;
#endif

void
JSRuntime::setGCTriggerFactor(uint32 factor)
{
    JS_ASSERT(factor >= 100);

    gcTriggerFactor = factor;
    setGCLastBytes(gcLastBytes);
}

void
JSRuntime::setGCLastBytes(size_t lastBytes)
{
    gcLastBytes = lastBytes;
    uint64 triggerBytes = uint64(lastBytes) * uint64(gcTriggerFactor / 100);
    if (triggerBytes != size_t(triggerBytes))
        triggerBytes = size_t(-1);
    gcTriggerBytes = size_t(triggerBytes);
}

static JS_INLINE bool
IsGCThresholdReached(JSRuntime *rt)
{
#ifdef JS_GC_ZEAL
    if (rt->gcZeal >= 1)
        return true;
#endif

    /*
     * Since the initial value of the gcLastBytes parameter is not equal to
     * zero (see the js_InitGC function) the return value is false when
     * the gcBytes value is close to zero at the JS engine start.
     */
    return rt->gcMallocBytes >= rt->gcMaxMallocBytes ||
           rt->gcBytes >= rt->gcTriggerBytes;
}

template <typename T, typename NewbornType>
static inline T*
NewFinalizableGCThing(JSContext *cx, unsigned thingKind,
                      NewbornType** newbornRoot)
{
    JSRuntime *rt;
    bool doGC;
    JSGCThing *thing;
    uint8 *flagp;
    JSGCArenaList *arenaList;
    JSGCArenaInfo *a;
    uintN thingsLimit;
    JSLocalRootStack *lrs;
#ifdef JS_GCMETER
    JSGCArenaStats *astats;
#endif
#ifdef JS_THREADSAFE
    JSBool gcLocked;
    uintN localMallocBytes;
    JSGCThing **lastptr;
    JSGCThing *tmpthing;
    uint8 *tmpflagp;
    uintN maxFreeThings;         /* max to take from the global free list */
#endif

    rt = cx->runtime;

    size_t nbytes = sizeof(T);
    JS_ASSERT(nbytes == GetFinalizableThingSize(thingKind));
    JS_ASSERT(JS_ROUNDUP(nbytes, sizeof(JSGCThing)) == nbytes);

    /* Updates of metering counters here may not be thread-safe. */
    METER(astats = &cx->runtime->gcStats.arenaStats[flindex]);
    METER(astats->alloc++);

#ifdef JS_THREADSAFE
    gcLocked = JS_FALSE;
    JS_ASSERT(cx->thread);

    JSGCThing *&freeList = cx->thread->gcFreeLists[thingKind];
    thing = freeList;
    localMallocBytes = JS_THREAD_DATA(cx)->gcMallocBytes;
    if (thing && rt->gcMaxMallocBytes - rt->gcMallocBytes > localMallocBytes) {
        flagp = thing->flagp;
        freeList = thing->next;
        METER(astats->localalloc++);
        goto success;
    }

    JS_LOCK_GC(rt);
    gcLocked = JS_TRUE;

    /* Transfer thread-local counter to global one. */
    if (localMallocBytes != 0) {
        JS_THREAD_DATA(cx)->gcMallocBytes = 0;
        if (rt->gcMaxMallocBytes - rt->gcMallocBytes < localMallocBytes)
            rt->gcMallocBytes = rt->gcMaxMallocBytes;
        else
            rt->gcMallocBytes += localMallocBytes;
    }
#endif
    JS_ASSERT(!rt->gcRunning);
    if (rt->gcRunning) {
        METER(rt->gcStats.finalfail++);
        JS_UNLOCK_GC(rt);
        return NULL;
    }

#if defined JS_GC_ZEAL && defined JS_TRACER
    if (rt->gcZeal >= 1 && JS_TRACE_MONITOR(cx).useReservedObjects)
        goto testReservedObjects;
#endif

    arenaList = &rt->gcArenaList[thingKind];
    doGC = IsGCThresholdReached(rt);
    for (;;) {
        if (doGC
#ifdef JS_TRACER
            && !JS_ON_TRACE(cx) && !JS_TRACE_MONITOR(cx).useReservedObjects
#endif
            ) {
            /*
             * Keep rt->gcLock across the call into js_GC so we don't starve
             * and lose to racing threads who deplete the heap just after
             * js_GC has replenished it (or has synchronized with a racing
             * GC that collected a bunch of garbage).  This unfair scheduling
             * can happen on certain operating systems. For the gory details,
             * see bug 162779 at https://bugzilla.mozilla.org/.
             */
            js_GC(cx, GC_LAST_DITCH);
            METER(astats->retry++);
        }

        /* Try to get thing from the free list. */
        thing = arenaList->freeList;
        if (thing) {
            arenaList->freeList = thing->next;
            flagp = thing->flagp;
            JS_ASSERT(*flagp & GCF_FINAL);

#ifdef JS_THREADSAFE
            /*
             * Refill the local free list by taking several things from the
             * global free list unless the free list is already populated or
             * we are still at rt->gcMaxMallocBytes barrier. The former is
             * caused via allocating new things in gcCallback(cx, JSGC_END).
             * The latter happens when GC is canceled due to
             * gcCallback(cx, JSGC_BEGIN) returning false.
             */
            if (freeList || rt->gcMallocBytes >= rt->gcMaxMallocBytes)
                break;

            tmpthing = arenaList->freeList;
            if (tmpthing) {
                maxFreeThings = MAX_THREAD_LOCAL_THINGS;
                do {
                    if (!tmpthing->next)
                        break;
                    tmpthing = tmpthing->next;
                } while (--maxFreeThings != 0);

                freeList = arenaList->freeList;
                arenaList->freeList = tmpthing->next;
                tmpthing->next = NULL;
            }
#endif
            break;
        }

        /*
         * Try to allocate things from the last arena. If it is fully used,
         * check if we can allocate a new one and, if we cannot, consider
         * doing a "last ditch" GC unless already tried.
         */
        thingsLimit = THINGS_PER_ARENA(nbytes);
        if (arenaList->lastCount != thingsLimit) {
            JS_ASSERT(arenaList->lastCount < thingsLimit);
            a = arenaList->last;
        } else {
#ifdef JS_TRACER
            if (JS_TRACE_MONITOR(cx).useReservedObjects) {
#ifdef JS_GC_ZEAL
testReservedObjects:
#endif
                JSTraceMonitor *tm = &JS_TRACE_MONITOR(cx);

                thing = (JSGCThing *) tm->reservedObjects;
                flagp = GetGCThingFlags(thing);
                JS_ASSERT(thing);
                tm->reservedObjects = JSVAL_TO_OBJECT(tm->reservedObjects->fslots[0]);
                break;
            }
#endif

            a = NewGCArena(rt);
            if (!a) {
                if (doGC || JS_ON_TRACE(cx))
                    goto fail;
                doGC = true;
                continue;
            }
            a->list = arenaList;
            a->prev = arenaList->last;
            a->prevUntracedPage = 0;
            a->u.untracedThings = 0;
            arenaList->last = a;
            arenaList->lastCount = 0;
        }

        flagp = THING_FLAGP(a, arenaList->lastCount);
        thing = FLAGP_TO_THING(flagp, nbytes);
        arenaList->lastCount++;

#ifdef JS_THREADSAFE
        /*
         * Refill the local free list by taking free things from the last
         * arena. Prefer to order free things by ascending address in the
         * (unscientific) hope of better cache locality.
         */
        if (freeList || rt->gcMallocBytes >= rt->gcMaxMallocBytes)
            break;
        lastptr = &freeList;
        maxFreeThings = thingsLimit - arenaList->lastCount;
        if (maxFreeThings > MAX_THREAD_LOCAL_THINGS)
            maxFreeThings = MAX_THREAD_LOCAL_THINGS;
        uint32 lastCount = arenaList->lastCount;
        while (maxFreeThings != 0) {
            --maxFreeThings;

            tmpflagp = THING_FLAGP(a, lastCount);
            tmpthing = FLAGP_TO_THING(tmpflagp, nbytes);
            lastCount++;
            tmpthing->flagp = tmpflagp;
            *tmpflagp = GCF_FINAL;    /* signifying that thing is free */

            *lastptr = tmpthing;
            lastptr = &tmpthing->next;
        }
        *lastptr = NULL;
        arenaList->lastCount = lastCount;
#endif
        break;
    }

    /* We successfully allocated the thing. */
#ifdef JS_THREADSAFE
  success:
#endif
    lrs = cx->localRootStack;
    if (lrs) {
        /*
         * If we're in a local root scope, don't set newborn[type] at all, to
         * avoid entraining garbage from it for an unbounded amount of time
         * on this context.  A caller will leave the local root scope and pop
         * this reference, allowing thing to be GC'd if it has no other refs.
         * See JS_EnterLocalRootScope and related APIs.
         */
        if (js_PushLocalRoot(cx, lrs, (jsval) thing) < 0) {
            /*
             * When we fail for a thing allocated through the tail of the last
             * arena, thing's flag byte is not initialized. So to prevent GC
             * accessing the uninitialized flags during the finalization, we
             * always mark the thing as final. See bug 337407.
             */
            *flagp = GCF_FINAL;
            goto fail;
        }
    } else {
        /*
         * No local root scope, so we're stuck with the old, fragile model of
         * depending on a pigeon-hole newborn per type per context.
         */
        *newbornRoot = (T *) thing;
    }

    /* We can't fail now, so update flags. */
    *flagp = uint8(0);

#ifdef DEBUG_gchist
    gchist[gchpos].lastDitch = doGC;
    gchist[gchpos].freeList = rt->gcArenaList[thingKind].freeList;
    if (++gchpos == NGCHIST)
        gchpos = 0;
#endif

    /* This is not thread-safe for thread-local allocations. */
    METER_IF(flags & GCF_LOCK, rt->gcStats.lockborn++);

#ifdef JS_THREADSAFE
    if (gcLocked)
        JS_UNLOCK_GC(rt);
#endif
    return (T*)thing;

fail:
#ifdef JS_THREADSAFE
    if (gcLocked)
        JS_UNLOCK_GC(rt);
#endif
    METER(astats->fail++);
    js_ReportOutOfMemory(cx);
    return NULL;
}

JSObject *
js_NewGCObject(JSContext *cx)
{
    return NewFinalizableGCThing<JSObject>(cx, FINALIZE_OBJECT,
                                           &cx->weakRoots.newbornObject);
}

JSString *
js_NewGCString(JSContext *cx)
{
    return NewFinalizableGCThing<JSString>(cx, FINALIZE_STRING,
                                           &cx->weakRoots.newbornString);
}

JSString *
js_NewGCExternalString(JSContext *cx, uintN type)
{
    JS_ASSERT(type < JS_EXTERNAL_STRING_LIMIT);
    return NewFinalizableGCThing<JSString>(cx, FINALIZE_EXTERNAL_STRING0 + type,
                                           &cx->weakRoots.newbornExternalString[type]);
}

JSFunction *
js_NewGCFunction(JSContext *cx)
{
    return NewFinalizableGCThing<JSFunction>(cx, FINALIZE_FUNCTION,
                                             &cx->weakRoots.newbornObject);
}

#if JS_HAS_XML_SUPPORT
JSXML *
js_NewGCXML(JSContext *cx)
{
    return NewFinalizableGCThing<JSXML>(cx,FINALIZE_XML,
                                        &cx->weakRoots.newbornXML);
}
#endif

static JSGCDoubleCell *
TurnUsedArenaIntoDoubleList(JSGCArenaInfo *a)
{
    JSGCDoubleCell *head;
    JSGCDoubleCell **tailp = &head;
    jsuword cellptr = ARENA_INFO_TO_START(a);

    /*
     * When m below points the last bitmap's word in the arena, its high bits
     * corresponds to non-existing cells and cellptr is outside the space
     * allocated for doubles. ClearDoubleArenaFlags sets such bits to 1. Thus
     * even for this last word its bit is unset iff the corresponding cell
     * exists and free.
     */
    for (jsbitmap *m = DOUBLE_ARENA_BITMAP(a);
         m != DOUBLE_ARENA_BITMAP_END(a);
         ++m) {
        JS_ASSERT(cellptr < reinterpret_cast<jsuword>(DOUBLE_ARENA_BITMAP(a)));
        JS_ASSERT((cellptr - ARENA_INFO_TO_START(a)) %
                  (JS_BITS_PER_WORD * sizeof(jsdouble)) == 0);

        jsbitmap bits = *m;
        if (bits == jsbitmap(-1)) {
            cellptr += JS_BITS_PER_WORD * sizeof(jsdouble);
        } else {
            /*
             * We have some zero bits. Turn corresponding cells into a list
             * unrolling the loop for better performance.
             */
            const unsigned unroll = 4;
            const jsbitmap unrollMask = (jsbitmap(1) << unroll) - 1;
            JS_STATIC_ASSERT((JS_BITS_PER_WORD & unrollMask) == 0);

            for (unsigned n = 0; n != JS_BITS_PER_WORD; n += unroll) {
                jsbitmap bitsChunk = bits & unrollMask;
                bits >>= unroll;
                if (bitsChunk == unrollMask) {
                    cellptr += unroll * sizeof(jsdouble);
                } else {
#define DO_BIT(bit)                                                           \
                    if (!(bitsChunk & (jsbitmap(1) << (bit)))) {              \
                        JS_ASSERT(cellptr - ARENA_INFO_TO_START(a) <=         \
                                  (DOUBLES_PER_ARENA - 1) * sizeof(jsdouble));\
                        JSGCDoubleCell *cell =                                \
                            reinterpret_cast<JSGCDoubleCell *>(cellptr);      \
                        *tailp = cell;                                        \
                        tailp = &cell->link;                                  \
                    }                                                         \
                    cellptr += sizeof(jsdouble);
                    DO_BIT(0);
                    DO_BIT(1);
                    DO_BIT(2);
                    DO_BIT(3);
#undef DO_BIT
                }
            }
        }
    }
    *tailp = NULL;
    return head;
}

static JSGCDoubleCell *
TurnEmptyArenaIntoDoubleList(JSGCArenaInfo *a)
{
    JSGCDoubleCell *head =
        reinterpret_cast<JSGCDoubleCell*>(ARENA_INFO_TO_START(a));
    JSGCDoubleCell *lastcell = head + DOUBLES_PER_ARENA - 1;

    for (JSGCDoubleCell *cell = head; cell != lastcell; ++cell)
        cell->link = cell + 1;

    lastcell->link = NULL;
    return head;
}

static JSGCDoubleCell *
RefillDoubleFreeList(JSContext *cx)
{
    JS_ASSERT(!JS_THREAD_DATA(cx)->doubleFreeList);

    JSRuntime *rt = cx->runtime;
    JS_ASSERT(!rt->gcRunning);

    JS_LOCK_GC(rt);

    JSGCArenaInfo *a;
    bool canGC = !JS_ON_TRACE(cx);
    bool doGC = canGC && IsGCThresholdReached(rt);
    for (;;) {
        if (doGC) {
            js_GC(cx, GC_LAST_DITCH);
            METER(rt->gcStats.doubleArenaStats.retry++);
            canGC = false;
        }

        /*
         * Loop until we find arena with some free doubles. We turn arenas
         * into free lists outside the lock to minimize contention between
         * threads.
         */
        while (!!(a = rt->gcDoubleArenaList.cursor)) {
            JS_ASSERT(!a->u.hasMarkedDoubles);
            rt->gcDoubleArenaList.cursor = a->prev;
            JS_UNLOCK_GC(rt);
            JSGCDoubleCell *list = TurnUsedArenaIntoDoubleList(a);
            if (list)
                return list;
            JS_LOCK_GC(rt);
        }
        a = NewGCArena(rt);
        if (a)
            break;
        if (!canGC) {
            METER(rt->gcStats.doubleArenaStats.fail++);
            JS_UNLOCK_GC(rt);
            js_ReportOutOfMemory(cx);
            return NULL;
        }
        doGC = true;
    }

    a->list = NULL;
    a->u.hasMarkedDoubles = false;
    a->prev = rt->gcDoubleArenaList.first;
    rt->gcDoubleArenaList.first = a;
    JS_UNLOCK_GC(rt);
    return TurnEmptyArenaIntoDoubleList(a);
}

JSBool
js_NewDoubleInRootedValue(JSContext *cx, jsdouble d, jsval *vp)
{
    /* Updates of metering counters here are not thread-safe. */
    METER(JSGCArenaStats *astats = &cx->runtime->gcStats.doubleArenaStats);
    METER(astats->alloc++);

    JSGCDoubleCell *cell = JS_THREAD_DATA(cx)->doubleFreeList;
    if (!cell) {
        cell = RefillDoubleFreeList(cx);
        if (!cell) {
            METER(astats->fail++);
            return false;
        }
    } else {
        METER(astats->localalloc++);
    }

    /*
     * The GC things on the free lists come from one arena and the things on
     * the free list are strictly in the ascending order.
     */
    JS_ASSERT_IF(cell->link,
                 THING_TO_ARENA(cell) == THING_TO_ARENA(cell->link));
    JS_ASSERT_IF(cell->link, cell < cell->link);

    JS_THREAD_DATA(cx)->doubleFreeList = cell->link;
    cell->number = d;
    *vp = DOUBLE_TO_JSVAL(&cell->number);
    return true;
}

jsdouble *
js_NewWeaklyRootedDouble(JSContext *cx, jsdouble d)
{
    jsval v;
    jsdouble *dp;

    if (!js_NewDoubleInRootedValue(cx, d, &v))
        return NULL;

    JS_ASSERT(JSVAL_IS_DOUBLE(v));
    dp = JSVAL_TO_DOUBLE(v);
    if (cx->localRootStack) {
        if (js_PushLocalRoot(cx, cx->localRootStack, v) < 0)
            return NULL;
    } else {
        cx->weakRoots.newbornDouble = dp;
    }
    return dp;
}

#ifdef JS_TRACER
JSBool
js_ReserveObjects(JSContext *cx, size_t nobjects)
{
    /*
     * Ensure at least nobjects objects are in the list. fslots[1] of each
     * object on the reservedObjects list is the length of the list from there.
     */
    JSObject *&head = JS_TRACE_MONITOR(cx).reservedObjects;
    size_t i = head ? JSVAL_TO_INT(head->fslots[1]) : 0;
    while (i < nobjects) {
        JSObject *obj = js_NewGCObject(cx);
        if (!obj)
            return JS_FALSE;
        memset(obj, 0, sizeof(JSObject));
        /* The class must be set to something for finalization. */
        obj->classword = (jsuword) &js_ObjectClass;
        obj->fslots[0] = OBJECT_TO_JSVAL(head);
        i++;
        obj->fslots[1] = INT_TO_JSVAL(i);
        head = obj;
    }

    return JS_TRUE;
}
#endif

/*
 * Shallow GC-things can be locked just by setting the GCF_LOCK bit, because
 * they have no descendants to mark during the GC. Currently the optimization
 * is only used for non-dependant strings.
 */
static uint8 *
GetShallowGCThingFlag(void *thing)
{
    if (JSString::isStatic(thing))
        return NULL;
    JSGCArenaInfo *a = THING_TO_ARENA(thing);
    if (!a->list || !IsFinalizableStringKind(a->list->thingKind))
        return NULL;

    JS_ASSERT(sizeof(JSString) == a->list->thingSize);
    JSString *str = (JSString *) thing;
    if (str->isDependent())
        return NULL;

    uint32 index = THING_TO_INDEX(thing, sizeof(JSString));
    return THING_FLAGP(a, index);
}

/* This is compatible with JSDHashEntryStub. */
typedef struct JSGCLockHashEntry {
    JSDHashEntryHdr hdr;
    const void      *thing;
    uint32          count;
} JSGCLockHashEntry;

JSBool
js_LockGCThingRT(JSRuntime *rt, void *thing)
{
    if (!thing)
        return JS_TRUE;

    JS_LOCK_GC(rt);

    /*
     * Avoid adding a rt->gcLocksHash entry for shallow things until someone
     * nests a lock.
     */
    uint8 *flagp = GetShallowGCThingFlag(thing);
    JSBool ok;
    JSGCLockHashEntry *lhe;
    if (flagp && !(*flagp & GCF_LOCK)) {
        *flagp |= GCF_LOCK;
        METER(rt->gcStats.lock++);
        ok = JS_TRUE;
        goto out;
    }

    if (!rt->gcLocksHash) {
        rt->gcLocksHash = JS_NewDHashTable(JS_DHashGetStubOps(), NULL,
                                           sizeof(JSGCLockHashEntry),
                                           GC_ROOTS_SIZE);
        if (!rt->gcLocksHash) {
            ok = JS_FALSE;
            goto out;
        }
    }

    lhe = (JSGCLockHashEntry *)
          JS_DHashTableOperate(rt->gcLocksHash, thing, JS_DHASH_ADD);
    if (!lhe) {
        ok = JS_FALSE;
        goto out;
    }
    if (!lhe->thing) {
        lhe->thing = thing;
        lhe->count = 1;
    } else {
        JS_ASSERT(lhe->count >= 1);
        lhe->count++;
    }

    METER(rt->gcStats.lock++);
    ok = JS_TRUE;
  out:
    JS_UNLOCK_GC(rt);
    return ok;
}

JSBool
js_UnlockGCThingRT(JSRuntime *rt, void *thing)
{
    if (!thing)
        return JS_TRUE;

    JS_LOCK_GC(rt);

    uint8 *flagp = GetShallowGCThingFlag(thing);
    JSGCLockHashEntry *lhe;
    if (flagp && !(*flagp & GCF_LOCK))
        goto out;
    if (!rt->gcLocksHash ||
        (lhe = (JSGCLockHashEntry *)
         JS_DHashTableOperate(rt->gcLocksHash, thing,
                              JS_DHASH_LOOKUP),
             JS_DHASH_ENTRY_IS_FREE(&lhe->hdr))) {
        /* Shallow entry is not in the hash -> clear its lock bit. */
        if (flagp)
            *flagp &= ~GCF_LOCK;
        else
            goto out;
    } else {
        if (--lhe->count != 0)
            goto out;
        JS_DHashTableOperate(rt->gcLocksHash, thing, JS_DHASH_REMOVE);
    }

    rt->gcPoke = JS_TRUE;
    METER(rt->gcStats.unlock++);
  out:
    JS_UNLOCK_GC(rt);
    return JS_TRUE;
}

JS_PUBLIC_API(void)
JS_TraceChildren(JSTracer *trc, void *thing, uint32 kind)
{
    switch (kind) {
      case JSTRACE_OBJECT: {
        /* If obj has no map, it must be a newborn. */
        JSObject *obj = (JSObject *) thing;
        if (!obj->map)
            break;
        obj->map->ops->trace(trc, obj);
        break;
      }

      case JSTRACE_STRING: {
        JSString *str = (JSString *) thing;
        if (str->isDependent())
            JS_CALL_STRING_TRACER(trc, str->dependentBase(), "base");
        break;
      }

#if JS_HAS_XML_SUPPORT
      case JSTRACE_XML:
        js_TraceXML(trc, (JSXML *)thing);
        break;
#endif
    }
}

/*
 * Number of things covered by a single bit of JSGCArenaInfo.u.untracedThings.
 */
#define THINGS_PER_UNTRACED_BIT(thingSize)                                    \
    JS_HOWMANY(THINGS_PER_ARENA(thingSize), JS_BITS_PER_WORD)

static void
DelayTracingChildren(JSRuntime *rt, uint8 *flagp)
{
    JSGCArenaInfo *a;
    uint32 untracedBitIndex;
    jsuword bit;

    /*
     * Things with children to be traced later are marked with
     * GCF_MARK | GCF_FINAL flags.
     */
    JS_ASSERT((*flagp & (GCF_MARK | GCF_FINAL)) == GCF_MARK);
    *flagp |= GCF_FINAL;

    METER(rt->gcStats.untraced++);
#ifdef DEBUG
    ++rt->gcTraceLaterCount;
    METER_UPDATE_MAX(rt->gcStats.maxuntraced, rt->gcTraceLaterCount);
#endif

    a = FLAGP_TO_ARENA(flagp);
    untracedBitIndex = FLAGP_TO_INDEX(flagp) /
                       THINGS_PER_UNTRACED_BIT(a->list->thingSize);
    JS_ASSERT(untracedBitIndex < JS_BITS_PER_WORD);
    bit = (jsuword)1 << untracedBitIndex;
    if (a->u.untracedThings != 0) {
        JS_ASSERT(rt->gcUntracedArenaStackTop);
        if (a->u.untracedThings & bit) {
            /* bit already covers things with children to trace later. */
            return;
        }
        a->u.untracedThings |= bit;
    } else {
        /*
         * The thing is the first thing with not yet traced children in the
         * whole arena, so push the arena on the stack of arenas with things
         * to be traced later unless the arena has already been pushed. We
         * detect that through checking prevUntracedPage as the field is 0
         * only for not yet pushed arenas. To ensure that
         *   prevUntracedPage != 0
         * even when the stack contains one element, we make prevUntracedPage
         * for the arena at the bottom to point to itself.
         *
         * See comments in TraceDelayedChildren.
         */
        a->u.untracedThings = bit;
        if (a->prevUntracedPage == 0) {
            if (!rt->gcUntracedArenaStackTop) {
                /* Stack was empty, mark the arena as the bottom element. */
                a->prevUntracedPage = ARENA_INFO_TO_PAGE(a);
            } else {
                JS_ASSERT(rt->gcUntracedArenaStackTop->prevUntracedPage != 0);
                a->prevUntracedPage =
                    ARENA_INFO_TO_PAGE(rt->gcUntracedArenaStackTop);
            }
            rt->gcUntracedArenaStackTop = a;
        }
    }
    JS_ASSERT(rt->gcUntracedArenaStackTop);
}

static void
TraceDelayedChildren(JSTracer *trc)
{
    JSRuntime *rt;
    JSGCArenaInfo *a, *aprev;
    uint32 thingSize, traceKind;
    uint32 thingsPerUntracedBit;
    uint32 untracedBitIndex, thingIndex, indexLimit, endIndex;
    JSGCThing *thing;
    uint8 *flagp;

    rt = trc->context->runtime;
    a = rt->gcUntracedArenaStackTop;
    if (!a) {
        JS_ASSERT(rt->gcTraceLaterCount == 0);
        return;
    }

    for (;;) {
        /*
         * The following assert verifies that the current arena belongs to the
         * untraced stack, since DelayTracingChildren ensures that even for
         * stack's bottom prevUntracedPage != 0 but rather points to itself.
         */
        JS_ASSERT(a->prevUntracedPage != 0);
        JS_ASSERT(rt->gcUntracedArenaStackTop->prevUntracedPage != 0);
        thingSize = a->list->thingSize;
        traceKind = GetFinalizableArenaTraceKind(a);
        indexLimit = (a == a->list->last)
                     ? a->list->lastCount
                     : THINGS_PER_ARENA(thingSize);
        thingsPerUntracedBit = THINGS_PER_UNTRACED_BIT(thingSize);

        /*
         * We cannot use do-while loop here as a->u.untracedThings can be zero
         * before the loop as a leftover from the previous iterations. See
         * comments after the loop.
         */
        while (a->u.untracedThings != 0) {
            untracedBitIndex = JS_FLOOR_LOG2W(a->u.untracedThings);
            a->u.untracedThings &= ~((jsuword)1 << untracedBitIndex);
            thingIndex = untracedBitIndex * thingsPerUntracedBit;
            endIndex = thingIndex + thingsPerUntracedBit;

            /*
             * endIndex can go beyond the last allocated thing as the real
             * limit can be "inside" the bit.
             */
            if (endIndex > indexLimit)
                endIndex = indexLimit;
            JS_ASSERT(thingIndex < indexLimit);

            do {
                /*
                 * Skip free or already traced things that share the bit
                 * with untraced ones.
                 */
                flagp = THING_FLAGP(a, thingIndex);
                if ((*flagp & (GCF_MARK|GCF_FINAL)) != (GCF_MARK|GCF_FINAL))
                    continue;
                *flagp &= ~GCF_FINAL;
#ifdef DEBUG
                JS_ASSERT(rt->gcTraceLaterCount != 0);
                --rt->gcTraceLaterCount;
#endif
                thing = FLAGP_TO_THING(flagp, thingSize);
                JS_TraceChildren(trc, thing, traceKind);
            } while (++thingIndex != endIndex);
        }

        /*
         * We finished tracing of all things in the the arena but we can only
         * pop it from the stack if the arena is the stack's top.
         *
         * When JS_TraceChildren from the above calls JS_CallTracer that in
         * turn on low C stack calls DelayTracingChildren and the latter
         * pushes new arenas to the untraced stack, we have to skip popping
         * of this arena until it becomes the top of the stack again.
         */
        if (a == rt->gcUntracedArenaStackTop) {
            aprev = ARENA_PAGE_TO_INFO(a->prevUntracedPage);
            a->prevUntracedPage = 0;
            if (a == aprev) {
                /*
                 * prevUntracedPage points to itself and we reached the
                 * bottom of the stack.
                 */
                break;
            }
            rt->gcUntracedArenaStackTop = a = aprev;
        } else {
            a = rt->gcUntracedArenaStackTop;
        }
    }
    JS_ASSERT(rt->gcUntracedArenaStackTop);
    JS_ASSERT(rt->gcUntracedArenaStackTop->prevUntracedPage == 0);
    rt->gcUntracedArenaStackTop = NULL;
    JS_ASSERT(rt->gcTraceLaterCount == 0);
}

JS_PUBLIC_API(void)
JS_CallTracer(JSTracer *trc, void *thing, uint32 kind)
{
    JSContext *cx;
    JSRuntime *rt;
    JSGCArenaInfo *a;
    uintN index;
    uint8 *flagp;

    JS_ASSERT(thing);
    JS_ASSERT(JS_IS_VALID_TRACE_KIND(kind));
    JS_ASSERT(trc->debugPrinter || trc->debugPrintArg);

    if (!IS_GC_MARKING_TRACER(trc)) {
        trc->callback(trc, thing, kind);
        goto out;
    }

    cx = trc->context;
    rt = cx->runtime;
    JS_ASSERT(rt->gcMarkingTracer == trc);
    JS_ASSERT(rt->gcLevel > 0);

    /*
     * Optimize for string and double as their size is known and their tracing
     * is not recursive.
     */
    switch (kind) {
      case JSTRACE_DOUBLE:
        a = THING_TO_ARENA(thing);
        JS_ASSERT(!a->list);
        if (!a->u.hasMarkedDoubles) {
            ClearDoubleArenaFlags(a);
            a->u.hasMarkedDoubles = true;
        }
        index = DOUBLE_THING_TO_INDEX(thing);
        JS_SET_BIT(DOUBLE_ARENA_BITMAP(a), index);
        goto out;

      case JSTRACE_STRING:
        for (;;) {
            if (JSString::isStatic(thing))
                goto out;
            a = THING_TO_ARENA(thing);
            flagp = THING_FLAGP(a, THING_TO_INDEX(thing, sizeof(JSString)));
            JS_ASSERT((*flagp & GCF_FINAL) == 0);
            JS_ASSERT(kind == GetFinalizableArenaTraceKind(a));
            if (!((JSString *) thing)->isDependent()) {
                *flagp |= GCF_MARK;
                goto out;
            }
            if (*flagp & GCF_MARK)
                goto out;
            *flagp |= GCF_MARK;
            thing = ((JSString *) thing)->dependentBase();
        }
        /* NOTREACHED */
    }

    JS_ASSERT(kind == GetFinalizableArenaTraceKind(THING_TO_ARENA(thing)));
    flagp = GetGCThingFlags(thing);
    if (*flagp & GCF_MARK)
        goto out;

    /*
     * We check for non-final flag only if mark is unset as
     * DelayTracingChildren uses the flag. See comments in the function.
     */
    JS_ASSERT(*flagp != GCF_FINAL);
    *flagp |= GCF_MARK;
    if (!cx->insideGCMarkCallback) {
        /*
         * With JS_GC_ASSUME_LOW_C_STACK defined the mark phase of GC always
         * uses the non-recursive code that otherwise would be called only on
         * a low C stack condition.
         */
#ifdef JS_GC_ASSUME_LOW_C_STACK
# define RECURSION_TOO_DEEP() JS_TRUE
#else
        int stackDummy;
# define RECURSION_TOO_DEEP() (!JS_CHECK_STACK_SIZE(cx, stackDummy))
#endif
        if (RECURSION_TOO_DEEP())
            DelayTracingChildren(rt, flagp);
        else
            JS_TraceChildren(trc, thing, kind);
    } else {
        /*
         * For API compatibility we allow for the callback to assume that
         * after it calls JS_MarkGCThing for the last time, the callback can
         * start to finalize its own objects that are only referenced by
         * unmarked GC things.
         *
         * Since we do not know which call from inside the callback is the
         * last, we ensure that children of all marked things are traced and
         * call TraceDelayedChildren(trc) after tracing the thing.
         *
         * As TraceDelayedChildren unconditionally invokes JS_TraceChildren
         * for the things with untraced children, calling DelayTracingChildren
         * is useless here. Hence we always trace thing's children even with a
         * low native stack.
         */
        cx->insideGCMarkCallback = JS_FALSE;
        JS_TraceChildren(trc, thing, kind);
        TraceDelayedChildren(trc);
        cx->insideGCMarkCallback = JS_TRUE;
    }

  out:
#ifdef DEBUG
    trc->debugPrinter = NULL;
    trc->debugPrintArg = NULL;
#endif
    return;     /* to avoid out: right_curl when DEBUG is not defined */
}

void
js_CallValueTracerIfGCThing(JSTracer *trc, jsval v)
{
    void *thing;
    uint32 kind;

    if (JSVAL_IS_DOUBLE(v) || JSVAL_IS_STRING(v)) {
        thing = JSVAL_TO_TRACEABLE(v);
        kind = JSVAL_TRACE_KIND(v);
        JS_ASSERT(kind == js_GetGCThingTraceKind(JSVAL_TO_GCTHING(v)));
    } else if (JSVAL_IS_OBJECT(v) && v != JSVAL_NULL) {
        /* v can be an arbitrary GC thing reinterpreted as an object. */
        thing = JSVAL_TO_OBJECT(v);
        kind = js_GetGCThingTraceKind(thing);
    } else {
        return;
    }
    JS_CallTracer(trc, thing, kind);
}

static JSDHashOperator
gc_root_traversal(JSDHashTable *table, JSDHashEntryHdr *hdr, uint32 num,
                  void *arg)
{
    JSGCRootHashEntry *rhe = (JSGCRootHashEntry *)hdr;
    JSTracer *trc = (JSTracer *)arg;
    jsval *rp = (jsval *)rhe->root;
    jsval v = *rp;

    /* Ignore null reference, scalar values, and static strings. */
    if (!JSVAL_IS_NULL(v) &&
        JSVAL_IS_GCTHING(v) &&
        !JSString::isStatic(JSVAL_TO_GCTHING(v))) {
#ifdef DEBUG
        bool root_points_to_gcArenaList = false;
        jsuword thing = (jsuword) JSVAL_TO_GCTHING(v);
        JSRuntime *rt = trc->context->runtime;
        for (unsigned i = 0; i != FINALIZE_LIMIT; i++) {
            JSGCArenaList *arenaList = &rt->gcArenaList[i];
            size_t thingSize = arenaList->thingSize;
            size_t limit = arenaList->lastCount * thingSize;
            for (JSGCArenaInfo *a = arenaList->last; a; a = a->prev) {
                if (thing - ARENA_INFO_TO_START(a) < limit) {
                    root_points_to_gcArenaList = true;
                    break;
                }
                limit = THINGS_PER_ARENA(thingSize) * thingSize;
            }
        }
        if (!root_points_to_gcArenaList) {
            for (JSGCArenaInfo *a = rt->gcDoubleArenaList.first; a; a = a->prev) {
                if (thing - ARENA_INFO_TO_START(a) <
                    DOUBLES_PER_ARENA * sizeof(jsdouble)) {
                    root_points_to_gcArenaList = true;
                    break;
                }
            }
        }
        if (!root_points_to_gcArenaList && rhe->name) {
            fprintf(stderr,
"JS API usage error: the address passed to JS_AddNamedRoot currently holds an\n"
"invalid jsval.  This is usually caused by a missing call to JS_RemoveRoot.\n"
"The root's name is \"%s\".\n",
                    rhe->name);
        }
        JS_ASSERT(root_points_to_gcArenaList);
#endif
        JS_SET_TRACING_NAME(trc, rhe->name ? rhe->name : "root");
        js_CallValueTracerIfGCThing(trc, v);
    }

    return JS_DHASH_NEXT;
}

static JSDHashOperator
gc_lock_traversal(JSDHashTable *table, JSDHashEntryHdr *hdr, uint32 num,
                  void *arg)
{
    JSGCLockHashEntry *lhe = (JSGCLockHashEntry *)hdr;
    void *thing = (void *)lhe->thing;
    JSTracer *trc = (JSTracer *)arg;
    uint32 traceKind;

    JS_ASSERT(lhe->count >= 1);
    traceKind = js_GetGCThingTraceKind(thing);
    JS_CALL_TRACER(trc, thing, traceKind, "locked object");
    return JS_DHASH_NEXT;
}

#define TRACE_JSVALS(trc, len, vec, name)                                     \
    JS_BEGIN_MACRO                                                            \
    jsval _v, *_vp, *_end;                                                    \
                                                                              \
        for (_vp = vec, _end = _vp + len; _vp < _end; _vp++) {                \
            _v = *_vp;                                                        \
            if (JSVAL_IS_TRACEABLE(_v)) {                                     \
                JS_SET_TRACING_INDEX(trc, name, _vp - (vec));                 \
                JS_CallTracer(trc, JSVAL_TO_TRACEABLE(_v),                    \
                              JSVAL_TRACE_KIND(_v));                          \
            }                                                                 \
        }                                                                     \
    JS_END_MACRO

void
js_TraceStackFrame(JSTracer *trc, JSStackFrame *fp)
{
    uintN nslots, minargs, skip;

    if (fp->callobj)
        JS_CALL_OBJECT_TRACER(trc, fp->callobj, "call");
    if (fp->argsobj)
        JS_CALL_OBJECT_TRACER(trc, JSVAL_TO_OBJECT(fp->argsobj), "arguments");
    if (fp->varobj)
        JS_CALL_OBJECT_TRACER(trc, fp->varobj, "variables");
    if (fp->script) {
        js_TraceScript(trc, fp->script);

        /* fp->slots is null for watch pseudo-frames, see js_watch_set. */
        if (fp->slots) {
            /*
             * Don't mark what has not been pushed yet, or what has been
             * popped already.
             */
            if (fp->regs && fp->regs->sp) {
                nslots = (uintN) (fp->regs->sp - fp->slots);
                JS_ASSERT(nslots >= fp->script->nfixed);
            } else {
                nslots = fp->script->nfixed;
            }
            TRACE_JSVALS(trc, nslots, fp->slots, "slot");
        }
    } else {
        JS_ASSERT(!fp->slots);
        JS_ASSERT(!fp->regs);
    }

    /* Allow for primitive this parameter due to JSFUN_THISP_* flags. */
    JS_CALL_VALUE_TRACER(trc, fp->thisv, "this");

    if (fp->argv) {
        JS_CALL_VALUE_TRACER(trc, fp->argv[-2], "callee");
        nslots = fp->argc;
        skip = 0;
        if (fp->fun) {
            minargs = FUN_MINARGS(fp->fun);
            if (minargs > nslots)
                nslots = minargs;
            if (!FUN_INTERPRETED(fp->fun)) {
                JS_ASSERT(!(fp->fun->flags & JSFUN_FAST_NATIVE));
                nslots += fp->fun->u.n.extra;
            }
            if (fp->fun->flags & JSFRAME_ROOTED_ARGV)
                skip = 2 + fp->argc;
        }
        TRACE_JSVALS(trc, 2 + nslots - skip, fp->argv - 2 + skip, "operand");
    }

    JS_CALL_VALUE_TRACER(trc, fp->rval, "rval");
    if (fp->scopeChain)
        JS_CALL_OBJECT_TRACER(trc, fp->scopeChain, "scope chain");
}

static void
TraceWeakRoots(JSTracer *trc, JSWeakRoots *wr)
{
    if (wr->newbornObject)
        JS_CALL_OBJECT_TRACER(trc, wr->newbornObject, "newborn_object");
    if (wr->newbornString)
        JS_CALL_STRING_TRACER(trc, wr->newbornString, "newborn_string");
    if (wr->newbornDouble)
        JS_CALL_DOUBLE_TRACER(trc, wr->newbornDouble, "newborn_double");
#if JS_HAS_XML_SUPPORT
    if (wr->newbornXML)
        JS_CALL_TRACER(trc, wr->newbornXML, JSTRACE_XML, "newborn_xml");
#endif
    for (uint32 i = 0; i != JS_EXTERNAL_STRING_LIMIT; i++) {
        JSString *str = wr->newbornExternalString[i];
        if (str)
            JS_CALL_STRING_TRACER(trc, str, "newborn_external_string");
    }
    JS_CALL_VALUE_TRACER(trc, wr->lastAtom, "lastAtom");
    JS_SET_TRACING_NAME(trc, "lastInternalResult");
    js_CallValueTracerIfGCThing(trc, wr->lastInternalResult);
}

JS_REQUIRES_STACK JS_FRIEND_API(void)
js_TraceContext(JSTracer *trc, JSContext *acx)
{
    JSStackFrame *fp, *nextChain;
    JSStackHeader *sh;
    JSTempValueRooter *tvr;

    if (IS_GC_MARKING_TRACER(trc)) {

#define FREE_OLD_ARENAS(pool)                                                 \
        JS_BEGIN_MACRO                                                        \
            int64 _age;                                                       \
            JSArena * _a = (pool).current;                                    \
            if (_a == (pool).first.next &&                                    \
                _a->avail == _a->base + sizeof(int64)) {                      \
                _age = JS_Now() - *(int64 *) _a->base;                        \
                if (_age > (int64) acx->runtime->gcEmptyArenaPoolLifespan *   \
                           1000)                                              \
                    JS_FreeArenaPool(&(pool));                                \
            }                                                                 \
        JS_END_MACRO

        /*
         * Release the stackPool's arenas if the stackPool has existed for
         * longer than the limit specified by gcEmptyArenaPoolLifespan.
         */
        FREE_OLD_ARENAS(acx->stackPool);

        /*
         * Release the regexpPool's arenas based on the same criterion as for
         * the stackPool.
         */
        FREE_OLD_ARENAS(acx->regexpPool);
    }

    /*
     * Iterate frame chain and dormant chains.
     *
     * (NB: see comment on this whole "dormant" thing in js_Execute.)
     *
     * Since js_GetTopStackFrame needs to dereference cx->thread to check for
     * JIT frames, we check for non-null thread here and avoid null checks
     * there. See bug 471197.
     */
#ifdef JS_THREADSAFE
    if (acx->thread)
#endif
    {
        fp = js_GetTopStackFrame(acx);
        nextChain = acx->dormantFrameChain;
        if (!fp)
            goto next_chain;

        /* The top frame must not be dormant. */
        JS_ASSERT(!fp->dormantNext);
        for (;;) {
            do {
                js_TraceStackFrame(trc, fp);
            } while ((fp = fp->down) != NULL);

          next_chain:
            if (!nextChain)
                break;
            fp = nextChain;
            nextChain = nextChain->dormantNext;
        }
    }

    /* Mark other roots-by-definition in acx. */
    if (acx->globalObject && !JS_HAS_OPTION(acx, JSOPTION_UNROOTED_GLOBAL))
        JS_CALL_OBJECT_TRACER(trc, acx->globalObject, "global object");
    TraceWeakRoots(trc, &acx->weakRoots);
    if (acx->throwing) {
        JS_CALL_VALUE_TRACER(trc, acx->exception, "exception");
    } else {
        /* Avoid keeping GC-ed junk stored in JSContext.exception. */
        acx->exception = JSVAL_NULL;
    }

    for (sh = acx->stackHeaders; sh; sh = sh->down) {
        METER(trc->context->runtime->gcStats.stackseg++);
        METER(trc->context->runtime->gcStats.segslots += sh->nslots);
        TRACE_JSVALS(trc, sh->nslots, JS_STACK_SEGMENT(sh), "stack");
    }

    if (acx->localRootStack)
        js_TraceLocalRoots(trc, acx->localRootStack);

    for (tvr = acx->tempValueRooters; tvr; tvr = tvr->down) {
        switch (tvr->count) {
          case JSTVU_SINGLE:
            JS_SET_TRACING_NAME(trc, "tvr->u.value");
            js_CallValueTracerIfGCThing(trc, tvr->u.value);
            break;
          case JSTVU_TRACE:
            tvr->u.trace(trc, tvr);
            break;
          case JSTVU_SPROP:
            tvr->u.sprop->trace(trc);
            break;
          case JSTVU_WEAK_ROOTS:
            TraceWeakRoots(trc, tvr->u.weakRoots);
            break;
          case JSTVU_COMPILER:
            tvr->u.compiler->trace(trc);
            break;
          case JSTVU_SCRIPT:
            js_TraceScript(trc, tvr->u.script);
            break;
          case JSTVU_ENUMERATOR:
            static_cast<JSAutoEnumStateRooter *>(tvr)->mark(trc);
            break;
          default:
            JS_ASSERT(tvr->count >= 0);
            TRACE_JSVALS(trc, tvr->count, tvr->u.array, "tvr->u.array");
        }
    }

    if (acx->sharpObjectMap.depth > 0)
        js_TraceSharpMap(trc, &acx->sharpObjectMap);

    js_TraceRegExpStatics(trc, acx);

#ifdef JS_TRACER
    InterpState* state = acx->interpState;
    while (state) {
        if (state->nativeVp)
            TRACE_JSVALS(trc, state->nativeVpLen, state->nativeVp, "nativeVp");
        state = state->prev;
    }
#endif
}

#ifdef JS_TRACER

static void
MarkReservedGCThings(JSTraceMonitor *tm)
{
    /* Keep reserved doubles. */
    for (jsval *ptr = tm->reservedDoublePool; ptr < tm->reservedDoublePoolPtr; ++ptr) {
        jsdouble* dp = JSVAL_TO_DOUBLE(*ptr);
        JS_ASSERT(js_GetGCThingTraceKind(dp) == JSTRACE_DOUBLE);

        JSGCArenaInfo *a = THING_TO_ARENA(dp);
        JS_ASSERT(!a->list);
        if (!a->u.hasMarkedDoubles) {
            ClearDoubleArenaFlags(a);
            a->u.hasMarkedDoubles = JS_TRUE;
        }
        jsuint index = DOUBLE_THING_TO_INDEX(dp);
        JS_SET_BIT(DOUBLE_ARENA_BITMAP(a), index);
    }
    /* Keep reserved objects. */
    for (JSObject *obj = tm->reservedObjects; obj; obj = JSVAL_TO_OBJECT(obj->fslots[0])) {
        JS_ASSERT(js_GetGCThingTraceKind(obj) == JSTRACE_OBJECT);

        uint8 *flagp = GetGCThingFlags(obj);
        JS_ASSERT(*flagp != GCF_FINAL);
        *flagp |= GCF_MARK;
    }
}

#ifdef JS_THREADSAFE
static JSDHashOperator
reserved_gcthings_marker(JSDHashTable *table, JSDHashEntryHdr *hdr,
                         uint32, void *)
{
    JSThread *thread = ((JSThreadsHashEntry *) hdr)->thread;

    MarkReservedGCThings(&thread->data.traceMonitor);
    return JS_DHASH_NEXT;
}
#endif

#endif

JS_REQUIRES_STACK void
js_TraceRuntime(JSTracer *trc, JSBool allAtoms)
{
    JSRuntime *rt = trc->context->runtime;
    JSContext *iter, *acx;

    JS_DHashTableEnumerate(&rt->gcRootsHash, gc_root_traversal, trc);
    if (rt->gcLocksHash)
        JS_DHashTableEnumerate(rt->gcLocksHash, gc_lock_traversal, trc);
    js_TraceAtomState(trc, allAtoms);
    js_TraceRuntimeNumberState(trc);

    iter = NULL;
    while ((acx = js_ContextIterator(rt, JS_TRUE, &iter)) != NULL)
        js_TraceContext(trc, acx);

    js_TraceThreads(rt, trc);

    if (rt->gcExtraRootsTraceOp)
        rt->gcExtraRootsTraceOp(trc, rt->gcExtraRootsData);

#ifdef JS_TRACER
    for (int i = 0; i < JSBUILTIN_LIMIT; i++) {
        if (rt->builtinFunctions[i])
            JS_CALL_OBJECT_TRACER(trc, rt->builtinFunctions[i], "builtin function");
    }

    /* Mark reserved gcthings unless we are shutting down. */
    if (IS_GC_MARKING_TRACER(trc) && rt->state != JSRTS_LANDING) {
#ifdef JS_THREADSAFE
        JS_DHashTableEnumerate(&rt->threads, reserved_gcthings_marker, NULL);
#else
        MarkReservedGCThings(&rt->threadData.traceMonitor);
#endif
    }

#endif
}

void
js_TriggerGC(JSContext *cx, JSBool gcLocked)
{
    JSRuntime *rt = cx->runtime;

#ifdef JS_THREADSAFE
    JS_ASSERT(cx->requestDepth > 0);
#endif
    JS_ASSERT(!rt->gcRunning);
    if (rt->gcIsNeeded)
        return;

    /*
     * Trigger the GC when it is safe to call an operation callback on any
     * thread.
     */
    rt->gcIsNeeded = JS_TRUE;
    js_TriggerAllOperationCallbacks(rt, gcLocked);
}

static void
ProcessSetSlotRequest(JSContext *cx, JSSetSlotRequest *ssr)
{
    JSObject *obj = ssr->obj;
    JSObject *pobj = ssr->pobj;
    uint32 slot = ssr->slot;

    while (pobj) {
        pobj = js_GetWrappedObject(cx, pobj);
        if (pobj == obj) {
            ssr->cycle = true;
            return;
        }
        pobj = JSVAL_TO_OBJECT(STOBJ_GET_SLOT(pobj, slot));
    }

    pobj = ssr->pobj;
    if (slot == JSSLOT_PROTO) {
        obj->setProto(pobj);
    } else {
        JS_ASSERT(slot == JSSLOT_PARENT);
        obj->setParent(pobj);
    }
}

void
js_DestroyScriptsToGC(JSContext *cx, JSThreadData *data)
{
    JSScript **listp, *script;

    for (size_t i = 0; i != JS_ARRAY_LENGTH(data->scriptsToGC); ++i) {
        listp = &data->scriptsToGC[i];
        while ((script = *listp) != NULL) {
            *listp = script->u.nextToGC;
            script->u.nextToGC = NULL;
            js_DestroyScript(cx, script);
        }
    }
}

static inline void
FinalizeGCThing(JSContext *cx, JSObject *obj, unsigned thingKind)
{
    JS_ASSERT(thingKind == FINALIZE_FUNCTION || thingKind == FINALIZE_OBJECT);

    /* Cope with stillborn objects that have no map. */
    if (!obj->map)
        return;

    if (JS_UNLIKELY(cx->debugHooks->objectHook != NULL)) {
        cx->debugHooks->objectHook(cx, obj, JS_FALSE,
                                   cx->debugHooks->objectHookData);
    }

    /* Finalize obj first, in case it needs map and slots. */
    JSClass *clasp = STOBJ_GET_CLASS(obj);
    if (clasp->finalize)
        clasp->finalize(cx, obj);

#ifdef INCLUDE_MOZILLA_DTRACE
    if (JAVASCRIPT_OBJECT_FINALIZE_ENABLED())
        jsdtrace_object_finalize(obj);
#endif

    if (JS_LIKELY(OBJ_IS_NATIVE(obj)))
        OBJ_SCOPE(obj)->drop(cx, obj);
    js_FreeSlots(cx, obj);
}

static inline void
FinalizeGCThing(JSContext *cx, JSFunction *fun, unsigned thingKind)
{
    JS_ASSERT(thingKind == FINALIZE_FUNCTION);
    FinalizeGCThing(cx, FUN_OBJECT(fun), thingKind);
}

#if JS_HAS_XML_SUPPORT
static inline void
FinalizeGCThing(JSContext *cx, JSXML *xml, unsigned thingKind)
{
    js_FinalizeXML(cx, xml);
}
#endif

JS_STATIC_ASSERT(JS_EXTERNAL_STRING_LIMIT == 8);
static JSStringFinalizeOp str_finalizers[JS_EXTERNAL_STRING_LIMIT] = {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

intN
js_ChangeExternalStringFinalizer(JSStringFinalizeOp oldop,
                                 JSStringFinalizeOp newop)
{
    for (uintN i = 0; i != JS_ARRAY_LENGTH(str_finalizers); i++) {
        if (str_finalizers[i] == oldop) {
            str_finalizers[i] = newop;
            return intN(i);
        }
    }
    return -1;
}

/*
 * cx is NULL when we are called from js_FinishAtomState to force the
 * finalization of the permanently interned strings.
 */
static void
FinalizeString(JSRuntime *rt, JSString *str, unsigned thingKind, JSContext *cx)
{
    jschar *chars;
    JSBool valid;
    JSStringFinalizeOp finalizer;

    JS_RUNTIME_UNMETER(rt, liveStrings);
    JS_ASSERT(!JSString::isStatic(str));
    JS_ASSERT(IsFinalizableStringKind(thingKind));
    if (str->isDependent()) {
        /* A dependent string can not be external and must be valid. */
        JS_ASSERT(thingKind == FINALIZE_STRING);
        JS_ASSERT(str->dependentBase());
        JS_RUNTIME_UNMETER(rt, liveDependentStrings);
        valid = JS_TRUE;
    } else {
        /* A stillborn string has null chars, so is not valid. */
        chars = str->flatChars();
        valid = (chars != NULL);
        if (valid) {
            if (thingKind == FINALIZE_STRING) {
                if (cx)
                    cx->free(chars);
                else
                    rt->free(chars);
            } else {
                unsigned type = thingKind - FINALIZE_EXTERNAL_STRING0;
                JS_ASSERT(type < JS_ARRAY_LENGTH(str_finalizers));
                finalizer = str_finalizers[type];
                if (finalizer) {
                    /*
                     * Assume that the finalizer for the permanently interned
                     * string knows how to deal with null context.
                     */
                    finalizer(cx, str);
                }
            }
        }
    }
    if (valid && str->isDeflated())
        js_PurgeDeflatedStringCache(rt, str);
}

static inline void
FinalizeGCThing(JSContext *cx, JSString *str, unsigned thingKind)
{
    return FinalizeString(cx->runtime, str, thingKind, cx);
}

void
js_FinalizeStringRT(JSRuntime *rt, JSString *str)
{
    JS_ASSERT(!JSString::isStatic(str));

    unsigned thingKind = THING_TO_ARENA(str)->list->thingKind;
    FinalizeString(rt, str, thingKind, NULL);
}

template<typename T>
static void
FinalizeArenaList(JSContext *cx, unsigned thingKind,
                  JSGCArenaInfo **emptyArenas)
{
    JSGCArenaList *arenaList = &cx->runtime->gcArenaList[thingKind];
    JS_ASSERT(sizeof(T) == arenaList->thingSize);

    JSGCArenaInfo **ap = &arenaList->last;
    JSGCArenaInfo *a = *ap;
    if (!a)
        return;

    JS_ASSERT(arenaList->lastCount > 0);

    JSGCThing *freeList = NULL;
    arenaList->freeList = NULL;

    uint32 indexLimit = THINGS_PER_ARENA(sizeof(T));
    uint8 *flagp = THING_FLAGP(a, arenaList->lastCount - 1);

#ifdef JS_GCMETER
    uint32 nlivearenas = 0, nkilledarenas = 0, nthings = 0;
#endif
    for (;;) {
        JS_ASSERT(a->prevUntracedPage == 0);
        JS_ASSERT(a->u.untracedThings == 0);

        bool allClear = true;
        do {
            if (*flagp & (GCF_MARK | GCF_LOCK)) {
                *flagp &= ~GCF_MARK;
                allClear = false;
                METER(nthings++);
            } else {
                JSGCThing *thing = FLAGP_TO_THING(flagp, sizeof(T));
                if (!(*flagp & GCF_FINAL)) {
                    /* Call the finalizer with GCF_FINAL ORed into flags. */
                    *flagp |= GCF_FINAL;
                    FinalizeGCThing(cx, (T *) thing, thingKind);
#ifdef DEBUG
                    memset(thing, JS_FREE_PATTERN, sizeof(T));
#endif
                }
                thing->flagp = flagp;
                thing->next = freeList;
                freeList = thing;
            }
        } while (++flagp != THING_FLAGS_END(a));

        if (allClear) {
            /*
             * Forget just assembled free list head for the arena and
             * add the arena itself to the destroy list.
             */
            freeList = arenaList->freeList;
            if (a == arenaList->last)
                arenaList->lastCount = indexLimit;
            *ap = a->prev;
            a->prev = *emptyArenas;
            *emptyArenas = a;
            METER(nkilledarenas++);
        } else {
            arenaList->freeList = freeList;
            ap = &a->prev;
            METER(nlivearenas++);
        }
        if (!(a = *ap))
            break;
        flagp = THING_FLAGP(a, indexLimit - 1);
    }

    METER(UpdateArenaStats(&rt->gcStats.arenaStats[thingKind],
                           nlivearenas, nkilledarenas, nthings));
}

/*
 * The gckind flag bit GC_LOCK_HELD indicates a call from js_NewGCThing with
 * rt->gcLock already held, so the lock should be kept on return.
 */
void
js_GC(JSContext *cx, JSGCInvocationKind gckind)
{
    JSRuntime *rt;
    JSBool keepAtoms;
    JSGCCallback callback;
    JSTracer trc;
    JSGCArenaInfo *emptyArenas, *a, **ap;
#ifdef JS_THREADSAFE
    uint32 requestDebit;
#endif
#ifdef JS_GCMETER
    uint32 nlivearenas, nkilledarenas, nthings;
#endif

    JS_ASSERT_IF(gckind == GC_LAST_DITCH, !JS_ON_TRACE(cx));
    rt = cx->runtime;

#ifdef JS_THREADSAFE
    /*
     * We allow js_GC calls outside a request but the context must be bound
     * to the current thread.
     */
    JS_ASSERT(CURRENT_THREAD_IS_ME(cx->thread));

    /* Avoid deadlock. */
    JS_ASSERT(!JS_IS_RUNTIME_LOCKED(rt));
#endif

    if (gckind & GC_KEEP_ATOMS) {
        /*
         * The set slot request and last ditch GC kinds preserve all atoms and
         * weak roots.
         */
        keepAtoms = JS_TRUE;
    } else {
        /* Keep atoms when a suspended compile is running on another context. */
        keepAtoms = (rt->gcKeepAtoms != 0);
        JS_CLEAR_WEAK_ROOTS(&cx->weakRoots);
    }

    /*
     * Don't collect garbage if the runtime isn't up, and cx is not the last
     * context in the runtime.  The last context must force a GC, and nothing
     * should suppress that final collection or there may be shutdown leaks,
     * or runtime bloat until the next context is created.
     */
    if (rt->state != JSRTS_UP && gckind != GC_LAST_CONTEXT)
        return;

  restart_at_beginning:
    /*
     * Let the API user decide to defer a GC if it wants to (unless this
     * is the last context).  Invoke the callback regardless. Sample the
     * callback in case we are freely racing with a JS_SetGCCallback{,RT} on
     * another thread.
     */
    if (gckind != GC_SET_SLOT_REQUEST && (callback = rt->gcCallback)) {
        JSBool ok;

        if (gckind & GC_LOCK_HELD)
            JS_UNLOCK_GC(rt);
        ok = callback(cx, JSGC_BEGIN);
        if (gckind & GC_LOCK_HELD)
            JS_LOCK_GC(rt);
        if (!ok && gckind != GC_LAST_CONTEXT) {
            /*
             * It's possible that we've looped back to this code from the 'goto
             * restart_at_beginning' below in the GC_SET_SLOT_REQUEST code and
             * that rt->gcLevel is now 0. Don't return without notifying!
             */
            if (rt->gcLevel == 0 && (gckind & GC_LOCK_HELD))
                JS_NOTIFY_GC_DONE(rt);
            return;
        }
    }

    /* Lock out other GC allocator and collector invocations. */
    if (!(gckind & GC_LOCK_HELD))
        JS_LOCK_GC(rt);

    METER(rt->gcStats.poke++);
    rt->gcPoke = JS_FALSE;

#ifdef JS_THREADSAFE
    /*
     * Check if the GC is already running on this or another thread and
     * delegate the job to it.
     */
    if (rt->gcLevel > 0) {
        JS_ASSERT(rt->gcThread);

        /* Bump gcLevel to restart the current GC, so it finds new garbage. */
        rt->gcLevel++;
        METER_UPDATE_MAX(rt->gcStats.maxlevel, rt->gcLevel);

        /*
         * If the GC runs on another thread, temporarily suspend the current
         * request and wait until the GC is done.
         */
        if (rt->gcThread != cx->thread) {
            requestDebit = js_DiscountRequestsForGC(cx);
            js_RecountRequestsAfterGC(rt, requestDebit);
        }
        if (!(gckind & GC_LOCK_HELD))
            JS_UNLOCK_GC(rt);
        return;
    }

    /* No other thread is in GC, so indicate that we're now in GC. */
    rt->gcLevel = 1;
    rt->gcThread = cx->thread;

    /*
     * Notify all operation callbacks, which will give them a chance to
     * yield their current request. Contexts that are not currently
     * executing will perform their callback at some later point,
     * which then will be unnecessary, but harmless.
     */
    js_NudgeOtherContexts(cx);

    /*
     * Discount all the requests on the current thread from contributing
     * to rt->requestCount before we wait for all other requests to finish.
     * JS_NOTIFY_REQUEST_DONE, which will wake us up, is only called on
     * rt->requestCount transitions to 0.
     */
    requestDebit = js_CountThreadRequests(cx);
    JS_ASSERT_IF(cx->requestDepth != 0, requestDebit >= 1);
    rt->requestCount -= requestDebit;
    while (rt->requestCount > 0)
        JS_AWAIT_REQUEST_DONE(rt);
    rt->requestCount += requestDebit;

#else  /* !JS_THREADSAFE */

    /* Bump gcLevel and return rather than nest; the outer gc will restart. */
    rt->gcLevel++;
    METER_UPDATE_MAX(rt->gcStats.maxlevel, rt->gcLevel);
    if (rt->gcLevel > 1)
        return;

#endif /* !JS_THREADSAFE */

    /*
     * Set rt->gcRunning here within the GC lock, and after waiting for any
     * active requests to end, so that new requests that try to JS_AddRoot,
     * JS_RemoveRoot, or JS_RemoveRootRT block in JS_BeginRequest waiting for
     * rt->gcLevel to drop to zero, while request-less calls to the *Root*
     * APIs block in js_AddRoot or js_RemoveRoot (see above in this file),
     * waiting for GC to finish.
     */
    rt->gcRunning = JS_TRUE;

    if (gckind == GC_SET_SLOT_REQUEST) {
        JSSetSlotRequest *ssr;

        while ((ssr = rt->setSlotRequests) != NULL) {
            rt->setSlotRequests = ssr->next;
            JS_UNLOCK_GC(rt);
            ssr->next = NULL;
            ProcessSetSlotRequest(cx, ssr);
            JS_LOCK_GC(rt);
        }

        /*
         * We assume here that killing links to parent and prototype objects
         * does not create garbage (such objects typically are long-lived and
         * widely shared, e.g. global objects, Function.prototype, etc.). We
         * collect garbage only if a racing thread attempted GC and is waiting
         * for us to finish (gcLevel > 1) or if someone already poked us.
         */
        if (rt->gcLevel == 1 && !rt->gcPoke && !rt->gcIsNeeded)
            goto done_running;

        rt->gcLevel = 0;
        rt->gcPoke = JS_FALSE;
        rt->gcRunning = JS_FALSE;
#ifdef JS_THREADSAFE
        rt->gcThread = NULL;
#endif
        gckind = GC_LOCK_HELD;
        goto restart_at_beginning;
    }

    JS_UNLOCK_GC(rt);

#ifdef JS_TRACER
    if (JS_ON_TRACE(cx))
        goto out;
#endif
    VOUCH_HAVE_STACK();

    /* Clear gcIsNeeded now, when we are about to start a normal GC cycle. */
    rt->gcIsNeeded = JS_FALSE;

    /* Reset malloc counter. */
    rt->gcMallocBytes = 0;

#ifdef JS_DUMP_SCOPE_METERS
  { extern void js_DumpScopeMeters(JSRuntime *rt);
    js_DumpScopeMeters(rt);
  }
#endif

#ifdef JS_TRACER
    js_PurgeJITOracle();
#endif

  restart:
    rt->gcNumber++;
    JS_ASSERT(!rt->gcUntracedArenaStackTop);
    JS_ASSERT(rt->gcTraceLaterCount == 0);

    /*
     * Reset the property cache's type id generator so we can compress ids.
     * Same for the protoHazardShape proxy-shape standing in for all object
     * prototypes having readonly or setter properties.
     */
    if (rt->shapeGen & SHAPE_OVERFLOW_BIT) {
        rt->gcRegenShapes = true;
        rt->gcRegenShapesScopeFlag ^= JSScope::SHAPE_REGEN;
        rt->shapeGen = 0;
        rt->protoHazardShape = 0;
    }

    js_PurgeThreads(cx);
#ifdef JS_TRACER
    if (gckind == GC_LAST_CONTEXT) {
        /* Clear builtin functions, which are recreated on demand. */
        memset(rt->builtinFunctions, 0, sizeof rt->builtinFunctions);
    }
#endif

    /*
     * Mark phase.
     */
    JS_TRACER_INIT(&trc, cx, NULL);
    rt->gcMarkingTracer = &trc;
    JS_ASSERT(IS_GC_MARKING_TRACER(&trc));

#ifdef DEBUG
    for (a = rt->gcDoubleArenaList.first; a; a = a->prev)
        JS_ASSERT(!a->u.hasMarkedDoubles);
#endif

    js_TraceRuntime(&trc, keepAtoms);
    js_MarkScriptFilenames(rt, keepAtoms);

    /*
     * Mark children of things that caused too deep recursion during the above
     * tracing.
     */
    TraceDelayedChildren(&trc);

    JS_ASSERT(!cx->insideGCMarkCallback);
    if (rt->gcCallback) {
        cx->insideGCMarkCallback = JS_TRUE;
        (void) rt->gcCallback(cx, JSGC_MARK_END);
        JS_ASSERT(cx->insideGCMarkCallback);
        cx->insideGCMarkCallback = JS_FALSE;
    }
    JS_ASSERT(rt->gcTraceLaterCount == 0);

    rt->gcMarkingTracer = NULL;

#ifdef JS_THREADSAFE
    cx->createDeallocatorTask();
#endif

    /*
     * Sweep phase.
     *
     * Finalize as we sweep, outside of rt->gcLock but with rt->gcRunning set
     * so that any attempt to allocate a GC-thing from a finalizer will fail,
     * rather than nest badly and leave the unmarked newborn to be swept.
     *
     * We first sweep atom state so we can use js_IsAboutToBeFinalized on
     * JSString or jsdouble held in a hashtable to check if the hashtable
     * entry can be freed. Note that even after the entry is freed, JSObject
     * finalizers can continue to access the corresponding jsdouble* and
     * JSString* assuming that they are unique. This works since the
     * atomization API must not be called during GC.
     */
    js_SweepAtomState(cx);

    /* Finalize iterator states before the objects they iterate over. */
    CloseNativeIterators(cx);

    /* Finalize watch points associated with unreachable objects. */
    js_SweepWatchPoints(cx);

#ifdef DEBUG
    /* Save the pre-sweep count of scope-mapped properties. */
    rt->liveScopePropsPreSweep = rt->liveScopeProps;
#endif

    /*
     * We finalize JSObject instances before JSString, double and other GC
     * things to ensure that object's finalizer can access them even if they
     * will be freed.
     *
     * To minimize the number of checks per each to be freed object and
     * function we use separated list finalizers when a debug hook is
     * installed.
     */
    emptyArenas = NULL;
    FinalizeArenaList<JSObject>(cx, FINALIZE_OBJECT, &emptyArenas);
    FinalizeArenaList<JSFunction>(cx, FINALIZE_FUNCTION, &emptyArenas);
#if JS_HAS_XML_SUPPORT
    FinalizeArenaList<JSXML>(cx, FINALIZE_XML, &emptyArenas);
#endif
    FinalizeArenaList<JSString>(cx, FINALIZE_STRING, &emptyArenas);
    for (unsigned i = FINALIZE_EXTERNAL_STRING0;
         i <= FINALIZE_EXTERNAL_STRING_LAST;
         ++i) {
        FinalizeArenaList<JSString>(cx, i, &emptyArenas);
    }

    ap = &rt->gcDoubleArenaList.first;
    METER((nlivearenas = 0, nkilledarenas = 0, nthings = 0));
    while ((a = *ap) != NULL) {
        if (!a->u.hasMarkedDoubles) {
            /* No marked double values in the arena. */
            *ap = a->prev;
            a->prev = emptyArenas;
            emptyArenas = a;
            METER(nkilledarenas++);
        } else {
            a->u.hasMarkedDoubles = false;
            ap = &a->prev;
#ifdef JS_GCMETER
            for (size_t i = 0; i != DOUBLES_PER_ARENA; ++i) {
                if (IsMarkedDouble(a, index))
                    METER(nthings++);
            }
            METER(nlivearenas++);
#endif
        }
    }
    METER(UpdateArenaStats(&rt->gcStats.doubleArenaStats,
                           nlivearenas, nkilledarenas, nthings));
    rt->gcDoubleArenaList.cursor = rt->gcDoubleArenaList.first;

    /*
     * Sweep the runtime's property tree after finalizing objects, in case any
     * had watchpoints referencing tree nodes.
     */
    js_SweepScopeProperties(cx);

    /*
     * Sweep script filenames after sweeping functions in the generic loop
     * above. In this way when a scripted function's finalizer destroys the
     * script and calls rt->destroyScriptHook, the hook can still access the
     * script's filename. See bug 323267.
     */
    js_SweepScriptFilenames(rt);

    /*
     * Destroy arenas after we finished the sweeping so finalizers can safely
     * use js_IsAboutToBeFinalized().
     */
    DestroyGCArenas(rt, emptyArenas);

#ifdef JS_THREADSAFE
    cx->submitDeallocatorTask();
#endif

    if (rt->gcCallback)
        (void) rt->gcCallback(cx, JSGC_FINALIZE_END);
#ifdef DEBUG_srcnotesize
  { extern void DumpSrcNoteSizeHist();
    DumpSrcNoteSizeHist();
    printf("GC HEAP SIZE %lu\n", (unsigned long)rt->gcBytes);
  }
#endif

#ifdef JS_SCOPE_DEPTH_METER
  { static FILE *fp;
    if (!fp)
        fp = fopen("/tmp/scopedepth.stats", "w");

    if (fp) {
        JS_DumpBasicStats(&rt->protoLookupDepthStats, "proto-lookup depth", fp);
        JS_DumpBasicStats(&rt->scopeSearchDepthStats, "scope-search depth", fp);
        JS_DumpBasicStats(&rt->hostenvScopeDepthStats, "hostenv scope depth", fp);
        JS_DumpBasicStats(&rt->lexicalScopeDepthStats, "lexical scope depth", fp);

        putc('\n', fp);
        fflush(fp);
    }
  }
#endif /* JS_SCOPE_DEPTH_METER */

#ifdef JS_DUMP_LOOP_STATS
  { static FILE *lsfp;
    if (!lsfp)
        lsfp = fopen("/tmp/loopstats", "w");
    if (lsfp) {
        JS_DumpBasicStats(&rt->loopStats, "loops", lsfp);
        fflush(lsfp);
    }
  }
#endif /* JS_DUMP_LOOP_STATS */

#ifdef JS_TRACER
out:
#endif
    JS_LOCK_GC(rt);

    /*
     * We want to restart GC if js_GC was called recursively or if any of the
     * finalizers called js_RemoveRoot or js_UnlockGCThingRT.
     */
    if (!JS_ON_TRACE(cx) && (rt->gcLevel > 1 || rt->gcPoke)) {
        VOUCH_HAVE_STACK();
        rt->gcLevel = 1;
        rt->gcPoke = JS_FALSE;
        JS_UNLOCK_GC(rt);
        goto restart;
    }

    rt->setGCLastBytes(rt->gcBytes);
  done_running:
    rt->gcLevel = 0;
    rt->gcRunning = rt->gcRegenShapes = false;

#ifdef JS_THREADSAFE
    rt->gcThread = NULL;
    JS_NOTIFY_GC_DONE(rt);

    /*
     * Unlock unless we have GC_LOCK_HELD which requires locked GC on return.
     */
    if (!(gckind & GC_LOCK_HELD))
        JS_UNLOCK_GC(rt);
#endif

    /*
     * Execute JSGC_END callback outside the lock. Again, sample the callback
     * pointer in case it changes, since we are outside of the GC vs. requests
     * interlock mechanism here.
     */
    if (gckind != GC_SET_SLOT_REQUEST && (callback = rt->gcCallback)) {
        JSWeakRoots savedWeakRoots;
        JSTempValueRooter tvr;

        if (gckind & GC_KEEP_ATOMS) {
            /*
             * We allow JSGC_END implementation to force a full GC or allocate
             * new GC things. Thus we must protect the weak roots from garbage
             * collection and overwrites.
             */
            savedWeakRoots = cx->weakRoots;
            JS_PUSH_TEMP_ROOT_WEAK_COPY(cx, &savedWeakRoots, &tvr);
            JS_KEEP_ATOMS(rt);
            JS_UNLOCK_GC(rt);
        }

        (void) callback(cx, JSGC_END);

        if (gckind & GC_KEEP_ATOMS) {
            JS_LOCK_GC(rt);
            JS_UNKEEP_ATOMS(rt);
            JS_POP_TEMP_ROOT(cx, &tvr);
        } else if (gckind == GC_LAST_CONTEXT && rt->gcPoke) {
            /*
             * On shutdown iterate until JSGC_END callback stops creating
             * garbage.
             */
            goto restart_at_beginning;
        }
    }
}
