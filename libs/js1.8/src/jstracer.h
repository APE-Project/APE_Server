/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99 ft=cpp:
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
 * The Original Code is Mozilla SpiderMonkey JavaScript 1.9 code, released
 * May 28, 2008.
 *
 * The Initial Developer of the Original Code is
 *   Brendan Eich <brendan@mozilla.org>
 *
 * Contributor(s):
 *   Andreas Gal <gal@mozilla.com>
 *   Mike Shaver <shaver@mozilla.org>
 *   David Anderson <danderson@mozilla.com>
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

#ifndef jstracer_h___
#define jstracer_h___

#ifdef JS_TRACER

#include "jstypes.h"
#include "jsbuiltins.h"
#include "jscntxt.h"
#include "jsdhash.h"
#include "jsinterp.h"
#include "jslock.h"
#include "jsnum.h"

#if defined(DEBUG) && !defined(JS_JIT_SPEW)
#define JS_JIT_SPEW
#endif

template <typename T>
class Queue {
    T* _data;
    unsigned _len;
    unsigned _max;
    nanojit::Allocator* alloc;

public:
    void ensure(unsigned size) {
        if (!_max)
            _max = 16;
        while (_max < size)
            _max <<= 1;
        if (alloc) {
            T* tmp = new (*alloc) T[_max];
            memcpy(tmp, _data, _len * sizeof(T));
            _data = tmp;
        } else {
            _data = (T*)realloc(_data, _max * sizeof(T));
        }
#if defined(DEBUG)
        memset(&_data[_len], 0xcd, _max - _len);
#endif
    }

    Queue(nanojit::Allocator* alloc)
        : alloc(alloc)
    {
        this->_max =
        this->_len = 0;
        this->_data = NULL;
    }

    ~Queue() {
        if (!alloc)
            free(_data);
    }

    bool contains(T a) {
        for (unsigned n = 0; n < _len; ++n) {
            if (_data[n] == a)
                return true;
        }
        return false;
    }

    void add(T a) {
        ensure(_len + 1);
        JS_ASSERT(_len <= _max);
        _data[_len++] = a;
    }

    void add(T* chunk, unsigned size) {
        ensure(_len + size);
        JS_ASSERT(_len <= _max);
        memcpy(&_data[_len], chunk, size * sizeof(T));
        _len += size;
    }

    void addUnique(T a) {
        if (!contains(a))
            add(a);
    }

    void setLength(unsigned len) {
        ensure(len + 1);
        _len = len;
    }

    void clear() {
        _len = 0;
    }

    T & get(unsigned i) {
        JS_ASSERT(i < length());
        return _data[i];
    }

    const T & get(unsigned i) const {
        JS_ASSERT(i < length());
        return _data[i];
    }

    T & operator [](unsigned i) {
        return get(i);
    }

    const T & operator [](unsigned i) const {
        return get(i);
    }

    unsigned length() const {
        return _len;
    }

    T* data() const {
        return _data;
    }
};

/*
 * Tracker is used to keep track of values being manipulated by the interpreter
 * during trace recording.  It maps opaque, 4-byte aligned address to LIns pointers.
 * pointers. To do this efficiently, we observe that the addresses of jsvals
 * living in the interpreter tend to be aggregated close to each other -
 * usually on the same page (where a tracker page doesn't have to be the same
 * size as the OS page size, but it's typically similar).  The Tracker
 * consists of a linked-list of structures representing a memory page, which
 * are created on-demand as memory locations are used.
 *
 * For every address, first we split it into two parts: upper bits which
 * represent the "base", and lower bits which represent an offset against the
 * base.  For the offset, we then right-shift it by two because the bottom two
 * bits of a 4-byte aligned address are always zero.  The mapping then
 * becomes:
 *
 *   page = page in pagelist such that Base(address) == page->base,
 *   page->map[Offset(address)]
 */
class Tracker {
    #define TRACKER_PAGE_SZB        4096
    #define TRACKER_PAGE_ENTRIES    (TRACKER_PAGE_SZB >> 2)    // each slot is 4 bytes
    #define TRACKER_PAGE_MASK       jsuword(TRACKER_PAGE_SZB - 1)

    struct TrackerPage {
        struct TrackerPage* next;
        jsuword             base;
        nanojit::LIns*      map[TRACKER_PAGE_ENTRIES];
    };
    struct TrackerPage* pagelist;

    jsuword             getTrackerPageBase(const void* v) const;
    jsuword             getTrackerPageOffset(const void* v) const;
    struct TrackerPage* findTrackerPage(const void* v) const;
    struct TrackerPage* addTrackerPage(const void* v);
public:
    Tracker();
    ~Tracker();

    bool            has(const void* v) const;
    nanojit::LIns*  get(const void* v) const;
    void            set(const void* v, nanojit::LIns* ins);
    void            clear();
};

#if defined(JS_JIT_SPEW) || defined(MOZ_NO_VARADIC_MACROS)

enum LC_TMBits {
    /*
     * Output control bits for all non-Nanojit code.  Only use bits 16 and
     * above, since Nanojit uses 0 .. 15 itself.
     */
    LC_TMMinimal  = 1<<16,
    LC_TMTracer   = 1<<17,
    LC_TMRecorder = 1<<18,
    LC_TMAbort    = 1<<19,
    LC_TMStats    = 1<<20,
    LC_TMRegexp   = 1<<21,
    LC_TMTreeVis  = 1<<22
};

#endif

#ifdef MOZ_NO_VARADIC_MACROS

#define debug_only_stmt(action)            /* */
static void debug_only_printf(int mask, const char *fmt, ...) {}
#define debug_only_print0(mask, str)       /* */

#elif defined(JS_JIT_SPEW)

// Top level logging controller object.
extern nanojit::LogControl js_LogController;

// Top level profiling hook, needed to harvest profile info from Fragments
// whose logical lifetime is about to finish
extern void js_FragProfiling_FragFinalizer(nanojit::Fragment* f, JSTraceMonitor*);

#define debug_only_stmt(stmt) \
    stmt

#define debug_only_printf(mask, fmt, ...)                                      \
    JS_BEGIN_MACRO                                                             \
        if ((js_LogController.lcbits & (mask)) > 0) {                          \
            js_LogController.printf(fmt, __VA_ARGS__);                         \
            fflush(stdout);                                                    \
        }                                                                      \
    JS_END_MACRO

#define debug_only_print0(mask, str)                                           \
    JS_BEGIN_MACRO                                                             \
        if ((js_LogController.lcbits & (mask)) > 0) {                          \
            js_LogController.printf("%s", str);                                \
            fflush(stdout);                                                    \
        }                                                                      \
    JS_END_MACRO

#else

#define debug_only_stmt(action)            /* */
#define debug_only_printf(mask, fmt, ...)  /* */
#define debug_only_print0(mask, str)       /* */

#endif

/*
 * The oracle keeps track of hit counts for program counter locations, as
 * well as slots that should not be demoted to int because we know them to
 * overflow or they result in type-unstable traces. We are using simple
 * hash tables.  Collisions lead to loss of optimization (demotable slots
 * are not demoted, etc.) but have no correctness implications.
 */
#define ORACLE_SIZE 4096

class Oracle {
    avmplus::BitSet _stackDontDemote;
    avmplus::BitSet _globalDontDemote;
    avmplus::BitSet _pcDontDemote;
public:
    Oracle();

    JS_REQUIRES_STACK void markGlobalSlotUndemotable(JSContext* cx, unsigned slot);
    JS_REQUIRES_STACK bool isGlobalSlotUndemotable(JSContext* cx, unsigned slot) const;
    JS_REQUIRES_STACK void markStackSlotUndemotable(JSContext* cx, unsigned slot);
    JS_REQUIRES_STACK void markStackSlotUndemotable(JSContext* cx, unsigned slot, const void* pc);
    JS_REQUIRES_STACK bool isStackSlotUndemotable(JSContext* cx, unsigned slot) const;
    JS_REQUIRES_STACK bool isStackSlotUndemotable(JSContext* cx, unsigned slot, const void* pc) const;
    void markInstructionUndemotable(jsbytecode* pc);
    bool isInstructionUndemotable(jsbytecode* pc) const;

    void clearDemotability();
    void clear() {
        clearDemotability();
    }
};

#if defined(_MSC_VER) && _MSC_VER >= 1400 || (defined(__GNUC__) && __GNUC__ >= 4)
#define USE_TRACE_TYPE_ENUM
#endif

/*
 * The types of values calculated during tracing, used to specialize operations
 * to the types of those values.  These loosely correspond to the values of the
 * JSVAL_* language types, but we add a few further divisions to enable further
 * optimization at execution time.  Do not rely on this loose correspondence for
 * correctness without adding static assertions!
 *
 * The ifdefs enforce that this enum occupies only one byte of memory, where
 * possible.  If it doesn't, type maps will occupy more space but should
 * otherwise work correctly.  A static assertion in jstracer.cpp verifies that
 * this requirement is correctly enforced by these compilers.
 */
enum JSTraceType_
#if defined(_MSC_VER) && _MSC_VER >= 1400
: int8_t
#endif
{
    TT_OBJECT         = 0, /* pointer to JSObject whose class is not js_FunctionClass */
    TT_INT32          = 1, /* 32-bit signed integer */
    TT_DOUBLE         = 2, /* pointer to jsdouble */
    TT_JSVAL          = 3, /* arbitrary jsval */
    TT_STRING         = 4, /* pointer to JSString */
    TT_NULL           = 5, /* null */
    TT_PSEUDOBOOLEAN  = 6, /* true, false, or undefined (0, 1, or 2) */
    TT_FUNCTION       = 7  /* pointer to JSObject whose class is js_FunctionClass */
}
#if defined(__GNUC__) && defined(USE_TRACE_TYPE_ENUM)
__attribute__((packed))
#endif
;

#ifdef USE_TRACE_TYPE_ENUM
typedef JSTraceType_ JSTraceType;
#else
typedef int8_t JSTraceType;
#endif

/*
 * This indicates an invalid type or error. Note that it should not be used in typemaps,
 * because it is the wrong size. It can only be used as a uint32, for example as the
 * return value from a function that returns a type as a uint32.
 */
const uint32 TT_INVALID = uint32(-1);

typedef Queue<uint16> SlotList;

class TypeMap : public Queue<JSTraceType> {
public:
    TypeMap(nanojit::Allocator* alloc) : Queue<JSTraceType>(alloc) {}
    JS_REQUIRES_STACK void captureTypes(JSContext* cx, JSObject* globalObj, SlotList& slots, unsigned callDepth);
    JS_REQUIRES_STACK void captureMissingGlobalTypes(JSContext* cx, JSObject* globalObj, SlotList& slots,
                                                     unsigned stackSlots);
    bool matches(TypeMap& other) const;
    void fromRaw(JSTraceType* other, unsigned numSlots);
};

#define JS_TM_EXITCODES(_)    \
    /*                                                                          \
     * An exit at a possible branch-point in the trace at which to attach a     \
     * future secondary trace. Therefore the recorder must generate different   \
     * code to handle the other outcome of the branch condition from the        \
     * primary trace's outcome.                                                 \
     */                                                                         \
    _(BRANCH)                                                                   \
    /*                                                                          \
     * Exit at a tableswitch via a numbered case.                               \
     */                                                                         \
    _(CASE)                                                                     \
    /*                                                                          \
     * Exit at a tableswitch via the default case.                              \
     */                                                                         \
    _(DEFAULT)                                                                  \
    _(LOOP)                                                                     \
    _(NESTED)                                                                   \
    /*                                                                          \
     * An exit from a trace because a condition relied upon at recording time   \
     * no longer holds, where the alternate path of execution is so rare or     \
     * difficult to address in native code that it is not traced at all, e.g.   \
     * negative array index accesses, which differ from positive indexes in     \
     * that they require a string-based property lookup rather than a simple    \
     * memory access.                                                           \
     */                                                                         \
    _(MISMATCH)                                                                 \
    /*                                                                          \
     * A specialization of MISMATCH_EXIT to handle allocation failures.         \
     */                                                                         \
    _(OOM)                                                                      \
    _(OVERFLOW)                                                                 \
    _(UNSTABLE_LOOP)                                                            \
    _(TIMEOUT)                                                                  \
    _(DEEP_BAIL)                                                                \
    _(STATUS)                                                                   \
    /* Exit is almost recursive and wants a peer at recursive_pc */             \
    _(RECURSIVE_UNLINKED)                                                       \
    /* Exit is recursive, and there are no more frames */                       \
    _(RECURSIVE_LOOP)                                                           \
    /* Exit is recursive, but type-mismatched guarding on a down frame */       \
    _(RECURSIVE_MISMATCH)                                                       \
    /* Exit is recursive, and the JIT wants to try slurping interp frames */    \
    _(RECURSIVE_EMPTY_RP)                                                       \
    /* Slurping interp frames in up-recursion failed */                         \
    _(RECURSIVE_SLURP_FAIL)                                                     \
    /* Tried to slurp an interp frame, but the pc or argc mismatched */         \
    _(RECURSIVE_SLURP_MISMATCH)

enum ExitType {
    #define MAKE_EXIT_CODE(x) x##_EXIT,
    JS_TM_EXITCODES(MAKE_EXIT_CODE)
    #undef MAKE_EXIT_CODE
    TOTAL_EXIT_TYPES
};

struct FrameInfo;

struct VMSideExit : public nanojit::SideExit
{
    JSObject* block;
    jsbytecode* pc;
    jsbytecode* imacpc;
    intptr_t sp_adj;
    intptr_t rp_adj;
    int32_t calldepth;
    uint32 numGlobalSlots;
    uint32 numStackSlots;
    uint32 numStackSlotsBelowCurrentFrame;
    ExitType exitType;
    uintN lookupFlags;
    void* recursive_pc;
    FrameInfo* recursive_down;
    unsigned hitcount;
    unsigned slurpFailSlot;
    JSTraceType slurpType;

    /*
     * Ordinarily 0.  If a slow native function is atop the stack, the 1 bit is
     * set if constructing and the other bits are a pointer to the funobj.
     */
    uintptr_t nativeCalleeWord;

    JSObject * nativeCallee() {
        return (JSObject *) (nativeCalleeWord & ~1);
    }

    bool constructing() {
        return bool(nativeCalleeWord & 1);
    }

    void setNativeCallee(JSObject *callee, bool constructing) {
        nativeCalleeWord = uintptr_t(callee) | (constructing ? 1 : 0);
    }

    inline JSTraceType* stackTypeMap() {
        return (JSTraceType*)(this + 1);
    }

    inline JSTraceType& stackType(unsigned i) {
        JS_ASSERT(i < numStackSlots);
        return stackTypeMap()[i];
    }

    inline JSTraceType* globalTypeMap() {
        return (JSTraceType*)(this + 1) + this->numStackSlots;
    }

    inline JSTraceType* fullTypeMap() {
        return stackTypeMap();
    }

    inline VMFragment* root() {
        return (VMFragment*)from->root;
    }
};

class VMAllocator : public nanojit::Allocator
{

public:
    VMAllocator() : mOutOfMemory(false), mSize(0)
    {}

    size_t size() {
        return mSize;
    }

    bool outOfMemory() {
        return mOutOfMemory;
    }

    struct Mark
    {
        VMAllocator& vma;
        bool committed;
        nanojit::Allocator::Chunk* saved_chunk;
        char* saved_top;
        char* saved_limit;
        size_t saved_size;

        Mark(VMAllocator& vma) :
            vma(vma),
            committed(false),
            saved_chunk(vma.current_chunk),
            saved_top(vma.current_top),
            saved_limit(vma.current_limit),
            saved_size(vma.mSize)
        {}

        ~Mark()
        {
            if (!committed)
                vma.rewind(*this);
        }

        void commit() { committed = true; }
    };

    void rewind(const Mark& m) {
        while (current_chunk != m.saved_chunk) {
            Chunk *prev = current_chunk->prev;
            freeChunk(current_chunk);
            current_chunk = prev;
        }
        current_top = m.saved_top;
        current_limit = m.saved_limit;
        mSize = m.saved_size;
        memset(current_top, 0, current_limit - current_top);
    }

    bool mOutOfMemory;
    size_t mSize;

    /*
     * FIXME: Area the LIR spills into if we encounter an OOM mid-way
     * through compilation; we must check mOutOfMemory before we run out
     * of mReserve, otherwise we're in undefined territory. This area
     * used to be one page, now 16 to be "safer". This is a temporary
     * and quite unsatisfactory approach to handling OOM in Nanojit.
     */
    uintptr_t mReserve[0x10000];
};


struct REHashKey {
    size_t re_length;
    uint16 re_flags;
    const jschar* re_chars;

    REHashKey(size_t re_length, uint16 re_flags, const jschar *re_chars)
        : re_length(re_length)
        , re_flags(re_flags)
        , re_chars(re_chars)
    {}

    bool operator==(const REHashKey& other) const
    {
        return ((this->re_length == other.re_length) &&
                (this->re_flags == other.re_flags) &&
                !memcmp(this->re_chars, other.re_chars,
                        this->re_length * sizeof(jschar)));
    }
};

struct REHashFn {
    static size_t hash(const REHashKey& k) {
        return
            k.re_length +
            k.re_flags +
            nanojit::murmurhash(k.re_chars, k.re_length * sizeof(jschar));
    }
};

class TreeInfo;

struct FrameInfo {
    JSObject*       block;      // caller block chain head
    jsbytecode*     pc;         // caller fp->regs->pc
    jsbytecode*     imacpc;     // caller fp->imacpc
    uint32          spdist;     // distance from fp->slots to fp->regs->sp at JSOP_CALL

    /*
     * Bit  15 (0x8000) is a flag that is set if constructing (called through new).
     * Bits 0-14 are the actual argument count. This may be less than fun->nargs.
     * NB: This is argc for the callee, not the caller.
     */
    uint32          argc;

    /*
     * Number of stack slots in the caller, not counting slots pushed when
     * invoking the callee. That is, slots after JSOP_CALL completes but
     * without the return value. This is also equal to the number of slots
     * between fp->down->argv[-2] (calleR fp->callee) and fp->argv[-2]
     * (calleE fp->callee).
     */
    uint32          callerHeight;

    /* argc of the caller */
    uint32          callerArgc;

    // Safer accessors for argc.
    enum { CONSTRUCTING_FLAG = 0x10000 };
    void   set_argc(uint16 argc, bool constructing) {
        this->argc = uint32(argc) | (constructing ? CONSTRUCTING_FLAG: 0);
    }
    uint16 get_argc() const { return uint16(argc & ~CONSTRUCTING_FLAG); }
    bool   is_constructing() const { return (argc & CONSTRUCTING_FLAG) != 0; }

    // The typemap just before the callee is called.
    JSTraceType* get_typemap() { return (JSTraceType*) (this+1); }
    const JSTraceType* get_typemap() const { return (JSTraceType*) (this+1); }
};

struct UnstableExit
{
    nanojit::Fragment* fragment;
    VMSideExit* exit;
    UnstableExit* next;
};

enum MonitorReason
{
    Monitor_Branch,
    Monitor_EnterFrame,
    Monitor_LeaveFrame
};

enum RecursionStatus
{
    Recursion_None,             /* No recursion has been compiled yet. */
    Recursion_Disallowed,       /* This tree cannot be recursive. */
    Recursion_Unwinds,          /* Tree is up-recursive only. */
    Recursion_Detected          /* Tree has down recursion and maybe up recursion. */
};

class TreeInfo {
public:
    nanojit::Fragment* const      fragment;
    JSScript*               script;
    unsigned                maxNativeStackSlots;
    ptrdiff_t               nativeStackBase;
    unsigned                maxCallDepth;
    TypeMap                 typeMap;
    unsigned                nStackTypes;
    SlotList*               globalSlots;
    /* Dependent trees must be trashed if this tree dies, and updated on missing global types */
    Queue<nanojit::Fragment*> dependentTrees;
    /* Linked trees must be updated on missing global types, but are not dependent */
    Queue<nanojit::Fragment*> linkedTrees;
    unsigned                branchCount;
    Queue<VMSideExit*>      sideExits;
    UnstableExit*           unstableExits;
    /* All embedded GC things are registered here so the GC can scan them. */
    Queue<jsval>            gcthings;
    Queue<JSScopeProperty*> sprops;
#ifdef DEBUG
    const char*             treeFileName;
    uintN                   treeLineNumber;
    uintN                   treePCOffset;
#endif
    RecursionStatus         recursion;

    TreeInfo(nanojit::Allocator* alloc,
             nanojit::Fragment* _fragment,
             SlotList* _globalSlots)
        : fragment(_fragment),
          script(NULL),
          maxNativeStackSlots(0),
          nativeStackBase(0),
          maxCallDepth(0),
          typeMap(alloc),
          nStackTypes(0),
          globalSlots(_globalSlots),
          dependentTrees(alloc),
          linkedTrees(alloc),
          branchCount(0),
          sideExits(alloc),
          unstableExits(NULL),
          gcthings(alloc),
          sprops(alloc),
          recursion(Recursion_None)
    {}

    inline unsigned nGlobalTypes() {
        return typeMap.length() - nStackTypes;
    }
    inline JSTraceType* globalTypeMap() {
        return typeMap.data() + nStackTypes;
    }
    inline JSTraceType* stackTypeMap() {
        return typeMap.data();
    }

    UnstableExit* removeUnstableExit(VMSideExit* exit);
};

#if defined(JS_JIT_SPEW) && (defined(NANOJIT_IA32) || defined(NANOJIT_X64))
# define EXECUTE_TREE_TIMER
#endif

typedef enum JSBuiltinStatus {
    JSBUILTIN_BAILED = 1,
    JSBUILTIN_ERROR = 2
} JSBuiltinStatus;

struct InterpState
{
    double        *sp;                  // native stack pointer, stack[0] is spbase[0]
    FrameInfo**   rp;                   // call stack pointer
    JSContext     *cx;                  // current VM context handle
    double        *eos;                 // first unusable word after the native stack
    void          *eor;                 // first unusable word after the call stack
    void          *sor;                 // start of rp stack
    VMSideExit*    lastTreeExitGuard;   // guard we exited on during a tree call
    VMSideExit*    lastTreeCallGuard;   // guard we want to grow from if the tree
                                        // call exit guard mismatched
    void*          rpAtLastTreeCall;    // value of rp at innermost tree call guard
    VMSideExit*    outermostTreeExitGuard; // the last side exit returned by js_CallTree
    TreeInfo*      outermostTree;       // the outermost tree we initially invoked
    double*        stackBase;           // native stack base
    FrameInfo**    callstackBase;       // call stack base
    uintN*         inlineCallCountp;    // inline call count counter
    VMSideExit**   innermostNestedGuardp;
    void*          stackMark;
    VMSideExit*    innermost;
#ifdef EXECUTE_TREE_TIMER
    uint64         startTime;
#endif
    InterpState*   prev;

    // Used by _FAIL builtins; see jsbuiltins.h. The builtin sets the
    // JSBUILTIN_BAILED bit if it bails off trace and the JSBUILTIN_ERROR bit
    // if an error or exception occurred.
    uint32         builtinStatus;

    // Used to communicate the location of the return value in case of a deep bail.
    double*        deepBailSp;


    // Used when calling natives from trace to root the vp vector.
    uintN          nativeVpLen;
    jsval         *nativeVp;
};

// Arguments objects created on trace have a private value that points to an
// instance of this struct. The struct includes a typemap that is allocated
// as part of the object.
struct js_ArgsPrivateNative {
    double      *argv;

    static js_ArgsPrivateNative *create(VMAllocator &alloc, unsigned argc)
    {
        return (js_ArgsPrivateNative*) new (alloc) char[sizeof(js_ArgsPrivateNative) + argc];
    }

    JSTraceType *typemap()
    {
        return (JSTraceType*) (this+1);
    }
};

static JS_INLINE void
js_SetBuiltinError(JSContext *cx)
{
    cx->interpState->builtinStatus |= JSBUILTIN_ERROR;
}

#ifdef DEBUG_RECORDING_STATUS_NOT_BOOL
/* #define DEBUG_RECORDING_STATUS_NOT_BOOL to detect misuses of RecordingStatus */
struct RecordingStatus {
    int code;
    bool operator==(RecordingStatus &s) { return this->code == s.code; };
    bool operator!=(RecordingStatus &s) { return this->code != s.code; };
};
enum RecordingStatusCodes {
    RECORD_ERROR_code     = 0,
    RECORD_STOP_code      = 1,

    RECORD_CONTINUE_code  = 3,
    RECORD_IMACRO_code    = 4
};
RecordingStatus RECORD_CONTINUE = { RECORD_CONTINUE_code };
RecordingStatus RECORD_STOP     = { RECORD_STOP_code };
RecordingStatus RECORD_IMACRO   = { RECORD_IMACRO_code };
RecordingStatus RECORD_ERROR    = { RECORD_ERROR_code };

struct AbortableRecordingStatus {
    int code;
    bool operator==(AbortableRecordingStatus &s) { return this->code == s.code; };
    bool operator!=(AbortableRecordingStatus &s) { return this->code != s.code; };
};
enum AbortableRecordingStatusCodes {
    ARECORD_ERROR_code    = 0,
    ARECORD_STOP_code     = 1,
    ARECORD_ABORTED_code  = 2,
    ARECORD_CONTINUE_code = 3,
    ARECORD_IMACRO_code   = 4
};
AbortableRecordingStatus ARECORD_ERROR    = { ARECORD_ERROR_code };
AbortableRecordingStatus ARECORD_STOP     = { ARECORD_STOP_code };
AbortableRecordingStatus ARECORD_CONTINUE = { ARECORD_CONTINUE_code };
AbortableRecordingStatus ARECORD_IMACRO   = { ARECORD_IMACRO_code };
AbortableRecordingStatus ARECORD_ABORTED =  { ARECORD_ABORTED_code };

static inline AbortableRecordingStatus
InjectStatus(RecordingStatus rs)
{
    AbortableRecordingStatus ars = { rs.code };
    return ars;
}
static inline AbortableRecordingStatus
InjectStatus(AbortableRecordingStatus ars)
{
    return ars;
}

static inline bool
StatusAbortsRecording(AbortableRecordingStatus ars)
{
    return ars == ARECORD_ERROR || ars == ARECORD_STOP || ars == ARECORD_ABORTED;
}
#else

/*
 * Normally, during recording, when the recorder cannot continue, it returns
 * ARECORD_STOP to indicate that recording should be aborted by the top-level
 * recording function. However, if the recorder reenters the interpreter (e.g.,
 * when executing an inner loop), there will be an immediate abort. This
 * condition must be carefully detected and propagated out of all nested
 * recorder calls lest the now-invalid TraceRecorder object be accessed
 * accidentally. This condition is indicated by the ARECORD_ABORTED value.
 *
 * The AbortableRecordingStatus enumeration represents the general set of
 * possible results of calling a recorder function. Functions that cannot
 * possibly return ARECORD_ABORTED may statically guarantee this to the caller
 * using the RecordingStatus enumeration. Ideally, C++ would allow subtyping
 * of enumerations, but it doesn't. To simulate subtype conversion manually,
 * code should call InjectStatus to inject a value of the restricted set into a
 * value of the general set.
 */

enum RecordingStatus {
    RECORD_ERROR      = 0,  // Error; propagate to interpreter.
    RECORD_STOP       = 1,  // Recording should be aborted at the top-level
                            // call to the recorder.
                            // (value reserved for ARECORD_ABORTED)
    RECORD_CONTINUE   = 3,  // Continue recording.
    RECORD_IMACRO     = 4   // Entered imacro; continue recording.
                            // Only JSOP_IS_IMACOP opcodes may return this.
};

enum AbortableRecordingStatus {
    ARECORD_ERROR    = 0,
    ARECORD_STOP     = 1,
    ARECORD_ABORTED  = 2,  // Recording has already been aborted; the recorder
                         // has been deleted.
    ARECORD_CONTINUE = 3,
    ARECORD_IMACRO   = 4
};

static JS_ALWAYS_INLINE AbortableRecordingStatus
InjectStatus(RecordingStatus rs)
{
    return static_cast<AbortableRecordingStatus>(rs);
}

static JS_ALWAYS_INLINE AbortableRecordingStatus
InjectStatus(AbortableRecordingStatus ars)
{
    return ars;
}

static JS_ALWAYS_INLINE bool
StatusAbortsRecording(AbortableRecordingStatus ars)
{
    return ars <= ARECORD_ABORTED;
}
#endif

class SlotMap;
class SlurpInfo;

/* Results of trying to compare two typemaps together */
enum TypeConsensus
{
    TypeConsensus_Okay,         /* Two typemaps are compatible */
    TypeConsensus_Undemotes,    /* Not compatible now, but would be with pending undemotes. */
    TypeConsensus_Bad           /* Typemaps are not compatible */
};

class TraceRecorder {
    VMAllocator&            tempAlloc;
    VMAllocator::Mark       mark;
    JSContext*              cx;
    JSTraceMonitor*         traceMonitor;
    JSObject*               globalObj;
    JSObject*               lexicalBlock;
    Tracker                 tracker;
    Tracker                 nativeFrameTracker;
    unsigned                callDepth;
    JSAtom**                atoms;
    VMSideExit*             anchor;
    nanojit::Fragment*      fragment;
    TreeInfo*               treeInfo;
    nanojit::LirBuffer*     lirbuf;
    nanojit::LirWriter*     lir;
    nanojit::LirBufWriter*  lir_buf_writer;
    nanojit::LirWriter*     verbose_filter;
    nanojit::LirWriter*     cse_filter;
    nanojit::LirWriter*     expr_filter;
    nanojit::LirWriter*     func_filter;
    nanojit::LirWriter*     float_filter;
#ifdef DEBUG
    nanojit::LirWriter*     sanity_filter_1;
    nanojit::LirWriter*     sanity_filter_2;
#endif
    nanojit::LIns*          cx_ins;
    nanojit::LIns*          eos_ins;
    nanojit::LIns*          eor_ins;
    nanojit::LIns*          rval_ins;
    nanojit::LIns*          inner_sp_ins;
    nanojit::LIns*          native_rval_ins;
    nanojit::LIns*          newobj_ins;
    bool                    trashSelf;
    Queue<nanojit::Fragment*> whichTreesToTrash;
    Queue<jsbytecode*>      cfgMerges;
    jsval*                  global_dslots;
    JSSpecializedNative     generatedSpecializedNative;
    JSSpecializedNative*    pendingSpecializedNative;
    jsval*                  pendingUnboxSlot;
    nanojit::LIns*          pendingGuardCondition;
    jsbytecode*             outer;     /* outer trace header PC */
    uint32                  outerArgc; /* outer trace deepest frame argc */
    bool                    loop;
    nanojit::LIns*          loopLabel;
    MonitorReason           monitorReason;

    nanojit::LIns* insImmObj(JSObject* obj);
    nanojit::LIns* insImmFun(JSFunction* fun);
    nanojit::LIns* insImmStr(JSString* str);
    nanojit::LIns* insImmSprop(JSScopeProperty* sprop);
    nanojit::LIns* p2i(nanojit::LIns* ins);

    bool isGlobal(jsval* p) const;
    ptrdiff_t nativeGlobalOffset(jsval* p) const;
    JS_REQUIRES_STACK ptrdiff_t nativeStackOffset(jsval* p) const;
    JS_REQUIRES_STACK void import(nanojit::LIns* base, ptrdiff_t offset, jsval* p, JSTraceType t,
                                  const char *prefix, uintN index, JSStackFrame *fp);
    JS_REQUIRES_STACK void import(TreeInfo* treeInfo, nanojit::LIns* sp, unsigned stackSlots,
                                  unsigned callDepth, unsigned ngslots, JSTraceType* typeMap);
    void trackNativeStackUse(unsigned slots);

    JS_REQUIRES_STACK bool isValidSlot(JSScope* scope, JSScopeProperty* sprop);
    JS_REQUIRES_STACK bool lazilyImportGlobalSlot(unsigned slot);

    JS_REQUIRES_STACK void guard(bool expected, nanojit::LIns* cond, ExitType exitType);
    JS_REQUIRES_STACK void guard(bool expected, nanojit::LIns* cond, VMSideExit* exit);
    JS_REQUIRES_STACK nanojit::LIns* slurpInt32Slot(nanojit::LIns* val_ins, jsval* vp,
                                                    VMSideExit* exit);
    JS_REQUIRES_STACK nanojit::LIns* slurpDoubleSlot(nanojit::LIns* val_ins, jsval* vp,
                                                     VMSideExit* exit);
    JS_REQUIRES_STACK nanojit::LIns* slurpStringSlot(nanojit::LIns* val_ins, jsval* vp,
                                                     VMSideExit* exit);
    JS_REQUIRES_STACK nanojit::LIns* slurpObjectSlot(nanojit::LIns* val_ins, jsval* vp,
                                                     VMSideExit* exit);
    JS_REQUIRES_STACK nanojit::LIns* slurpFunctionSlot(nanojit::LIns* val_ins, jsval* vp,
                                                       VMSideExit* exit);
    JS_REQUIRES_STACK nanojit::LIns* slurpNullSlot(nanojit::LIns* val_ins, jsval* vp,
                                                   VMSideExit* exit);
    JS_REQUIRES_STACK nanojit::LIns* slurpBoolSlot(nanojit::LIns* val_ins, jsval* vp,
                                                   VMSideExit* exit);
    JS_REQUIRES_STACK nanojit::LIns* slurpSlot(nanojit::LIns* val_ins, jsval* vp,
                                               VMSideExit* exit);
    JS_REQUIRES_STACK void slurpSlot(nanojit::LIns* val_ins, jsval* vp, SlurpInfo* info);
    JS_REQUIRES_STACK AbortableRecordingStatus slurpDownFrames(jsbytecode* return_pc);

    nanojit::LIns* addName(nanojit::LIns* ins, const char* name);

    nanojit::LIns* writeBack(nanojit::LIns* i, nanojit::LIns* base, ptrdiff_t offset,
                             bool demote);
    JS_REQUIRES_STACK void set(jsval* p, nanojit::LIns* l, bool initializing = false,
                               bool demote = true);
    JS_REQUIRES_STACK nanojit::LIns* get(jsval* p);
    JS_REQUIRES_STACK nanojit::LIns* addr(jsval* p);

    JS_REQUIRES_STACK bool known(jsval* p);
    JS_REQUIRES_STACK void checkForGlobalObjectReallocation();

    JS_REQUIRES_STACK TypeConsensus selfTypeStability(SlotMap& smap);
    JS_REQUIRES_STACK TypeConsensus peerTypeStability(SlotMap& smap, const void* ip,
                                                      VMFragment** peer);

    JS_REQUIRES_STACK jsval& argval(unsigned n) const;
    JS_REQUIRES_STACK jsval& varval(unsigned n) const;
    JS_REQUIRES_STACK jsval& stackval(int n) const;

    struct NameResult {
        // |tracked| is true iff the result of the name lookup is a variable that
        // is already in the tracker. The rest of the fields are set only if
        // |tracked| is false.
        bool             tracked;
        jsval            v;              // current property value
        JSObject         *obj;           // Call object where name was found
        nanojit::LIns    *obj_ins;       // LIR value for obj
        JSScopeProperty  *sprop;         // sprop name was resolved to
    };

    JS_REQUIRES_STACK nanojit::LIns* scopeChain() const;
    JS_REQUIRES_STACK JSStackFrame* frameIfInRange(JSObject* obj, unsigned* depthp = NULL) const;
    JS_REQUIRES_STACK RecordingStatus traverseScopeChain(JSObject *obj, nanojit::LIns *obj_ins, JSObject *obj2, nanojit::LIns *&obj2_ins);
    JS_REQUIRES_STACK AbortableRecordingStatus scopeChainProp(JSObject* obj, jsval*& vp, nanojit::LIns*& ins, NameResult& nr);
    JS_REQUIRES_STACK RecordingStatus callProp(JSObject* obj, JSProperty* sprop, jsid id, jsval*& vp, nanojit::LIns*& ins, NameResult& nr);

    JS_REQUIRES_STACK nanojit::LIns* arg(unsigned n);
    JS_REQUIRES_STACK void arg(unsigned n, nanojit::LIns* i);
    JS_REQUIRES_STACK nanojit::LIns* var(unsigned n);
    JS_REQUIRES_STACK void var(unsigned n, nanojit::LIns* i);
    JS_REQUIRES_STACK nanojit::LIns* upvar(JSScript* script, JSUpvarArray* uva, uintN index, jsval& v);
    nanojit::LIns* stackLoad(nanojit::LIns* addr, uint8 type);
    JS_REQUIRES_STACK nanojit::LIns* stack(int n);
    JS_REQUIRES_STACK void stack(int n, nanojit::LIns* i);

    JS_REQUIRES_STACK nanojit::LIns* alu(nanojit::LOpcode op, jsdouble v0, jsdouble v1,
                                         nanojit::LIns* s0, nanojit::LIns* s1);
    nanojit::LIns* f2i(nanojit::LIns* f);
    JS_REQUIRES_STACK nanojit::LIns* makeNumberInt32(nanojit::LIns* f);
    JS_REQUIRES_STACK nanojit::LIns* stringify(jsval& v);

    JS_REQUIRES_STACK nanojit::LIns* newArguments();

    JS_REQUIRES_STACK RecordingStatus call_imacro(jsbytecode* imacro);

    JS_REQUIRES_STACK AbortableRecordingStatus ifop();
    JS_REQUIRES_STACK RecordingStatus switchop();
#ifdef NANOJIT_IA32
    JS_REQUIRES_STACK AbortableRecordingStatus tableswitch();
#endif
    JS_REQUIRES_STACK RecordingStatus inc(jsval& v, jsint incr, bool pre = true);
    JS_REQUIRES_STACK RecordingStatus inc(jsval v, nanojit::LIns*& v_ins, jsint incr,
                                            bool pre = true);
    JS_REQUIRES_STACK RecordingStatus incHelper(jsval v, nanojit::LIns* v_ins,
                                                  nanojit::LIns*& v_after, jsint incr);
    JS_REQUIRES_STACK AbortableRecordingStatus incProp(jsint incr, bool pre = true);
    JS_REQUIRES_STACK RecordingStatus incElem(jsint incr, bool pre = true);
    JS_REQUIRES_STACK AbortableRecordingStatus incName(jsint incr, bool pre = true);

    JS_REQUIRES_STACK void strictEquality(bool equal, bool cmpCase);
    JS_REQUIRES_STACK AbortableRecordingStatus equality(bool negate, bool tryBranchAfterCond);
    JS_REQUIRES_STACK AbortableRecordingStatus equalityHelper(jsval l, jsval r,
                                                                nanojit::LIns* l_ins, nanojit::LIns* r_ins,
                                                                bool negate, bool tryBranchAfterCond,
                                                                jsval& rval);
    JS_REQUIRES_STACK AbortableRecordingStatus relational(nanojit::LOpcode op, bool tryBranchAfterCond);

    JS_REQUIRES_STACK RecordingStatus unary(nanojit::LOpcode op);
    JS_REQUIRES_STACK RecordingStatus binary(nanojit::LOpcode op);

    JS_REQUIRES_STACK RecordingStatus guardShape(nanojit::LIns* obj_ins, JSObject* obj,
                                                 uint32 shape, const char* name,
                                                 nanojit::LIns* map_ins, VMSideExit* exit);

    JSDHashTable guardedShapeTable;

#if defined DEBUG_notme && defined XP_UNIX
    void dumpGuardedShapes(const char* prefix);
#endif

    void forgetGuardedShapes();

    inline nanojit::LIns* map(nanojit::LIns *obj_ins);
    JS_REQUIRES_STACK bool map_is_native(JSObjectMap* map, nanojit::LIns* map_ins,
                                         nanojit::LIns*& ops_ins, size_t op_offset = 0);
    JS_REQUIRES_STACK AbortableRecordingStatus test_property_cache(JSObject* obj, nanojit::LIns* obj_ins,
                                                                     JSObject*& obj2, jsuword& pcval);
    JS_REQUIRES_STACK RecordingStatus guardNativePropertyOp(JSObject* aobj,
                                                              nanojit::LIns* map_ins);
    JS_REQUIRES_STACK RecordingStatus guardPropertyCacheHit(nanojit::LIns* obj_ins,
                                                              nanojit::LIns* map_ins,
                                                              JSObject* aobj,
                                                              JSObject* obj2,
                                                              JSPropCacheEntry* entry,
                                                              jsuword& pcval);

    void stobj_set_fslot(nanojit::LIns *obj_ins, unsigned slot,
                         nanojit::LIns* v_ins);
    void stobj_set_dslot(nanojit::LIns *obj_ins, unsigned slot, nanojit::LIns*& dslots_ins,
                         nanojit::LIns* v_ins);
    void stobj_set_slot(nanojit::LIns* obj_ins, unsigned slot, nanojit::LIns*& dslots_ins,
                        nanojit::LIns* v_ins);

    nanojit::LIns* stobj_get_fslot(nanojit::LIns* obj_ins, unsigned slot);
    nanojit::LIns* stobj_get_dslot(nanojit::LIns* obj_ins, unsigned index,
                                   nanojit::LIns*& dslots_ins);
    nanojit::LIns* stobj_get_slot(nanojit::LIns* obj_ins, unsigned slot,
                                  nanojit::LIns*& dslots_ins);

    nanojit::LIns* stobj_get_private(nanojit::LIns* obj_ins) {
        return stobj_get_fslot(obj_ins, JSSLOT_PRIVATE);
    }

    nanojit::LIns* stobj_get_proto(nanojit::LIns* obj_ins) {
        return stobj_get_fslot(obj_ins, JSSLOT_PROTO);
    }

    nanojit::LIns* stobj_get_parent(nanojit::LIns* obj_ins) {
        return stobj_get_fslot(obj_ins, JSSLOT_PARENT);
    }

    nanojit::LIns* getStringLength(nanojit::LIns* str_ins);

    JS_REQUIRES_STACK AbortableRecordingStatus name(jsval*& vp, nanojit::LIns*& ins, NameResult& nr);
    JS_REQUIRES_STACK AbortableRecordingStatus prop(JSObject* obj, nanojit::LIns* obj_ins, uint32 *slotp,
                                                      nanojit::LIns** v_insp, jsval* outp);
    JS_REQUIRES_STACK RecordingStatus denseArrayElement(jsval& oval, jsval& idx, jsval*& vp,
                                                          nanojit::LIns*& v_ins,
                                                          nanojit::LIns*& addr_ins);
    JS_REQUIRES_STACK AbortableRecordingStatus getProp(JSObject* obj, nanojit::LIns* obj_ins);
    JS_REQUIRES_STACK AbortableRecordingStatus getProp(jsval& v);
    JS_REQUIRES_STACK RecordingStatus getThis(nanojit::LIns*& this_ins);

    JS_REQUIRES_STACK VMSideExit* enterDeepBailCall();
    JS_REQUIRES_STACK void leaveDeepBailCall();

    JS_REQUIRES_STACK RecordingStatus primitiveToStringInPlace(jsval* vp);
    JS_REQUIRES_STACK void finishGetProp(nanojit::LIns* obj_ins, nanojit::LIns* vp_ins,
                                         nanojit::LIns* ok_ins, jsval* outp);
    JS_REQUIRES_STACK RecordingStatus getPropertyByName(nanojit::LIns* obj_ins, jsval* idvalp,
                                                          jsval* outp);
    JS_REQUIRES_STACK RecordingStatus getPropertyByIndex(nanojit::LIns* obj_ins,
                                                           nanojit::LIns* index_ins, jsval* outp);
    JS_REQUIRES_STACK RecordingStatus getPropertyById(nanojit::LIns* obj_ins, jsval* outp);
    JS_REQUIRES_STACK RecordingStatus getPropertyWithNativeGetter(nanojit::LIns* obj_ins,
                                                                    JSScopeProperty* sprop,
                                                                    jsval* outp);

    JS_REQUIRES_STACK RecordingStatus nativeSet(JSObject* obj, nanojit::LIns* obj_ins,
                                                  JSScopeProperty* sprop,
                                                  jsval v, nanojit::LIns* v_ins);
    JS_REQUIRES_STACK RecordingStatus setProp(jsval &l, JSPropCacheEntry* entry,
                                                JSScopeProperty* sprop,
                                                jsval &v, nanojit::LIns*& v_ins);
    JS_REQUIRES_STACK RecordingStatus setCallProp(JSObject *callobj, nanojit::LIns *callobj_ins,
                                                    JSScopeProperty *sprop, nanojit::LIns *v_ins,
                                                    jsval v);
    JS_REQUIRES_STACK RecordingStatus initOrSetPropertyByName(nanojit::LIns* obj_ins,
                                                                jsval* idvalp, jsval* rvalp,
                                                                bool init);
    JS_REQUIRES_STACK RecordingStatus initOrSetPropertyByIndex(nanojit::LIns* obj_ins,
                                                                 nanojit::LIns* index_ins,
                                                                 jsval* rvalp, bool init);

    JS_REQUIRES_STACK nanojit::LIns* box_jsval(jsval v, nanojit::LIns* v_ins);
    JS_REQUIRES_STACK nanojit::LIns* unbox_jsval(jsval v, nanojit::LIns* v_ins, VMSideExit* exit);
    JS_REQUIRES_STACK bool guardClass(JSObject* obj, nanojit::LIns* obj_ins, JSClass* clasp,
                                      VMSideExit* exit);
    JS_REQUIRES_STACK bool guardDenseArray(JSObject* obj, nanojit::LIns* obj_ins,
                                           ExitType exitType = MISMATCH_EXIT);
    JS_REQUIRES_STACK bool guardDenseArray(JSObject* obj, nanojit::LIns* obj_ins,
                                           VMSideExit* exit);
    JS_REQUIRES_STACK bool guardHasPrototype(JSObject* obj, nanojit::LIns* obj_ins,
                                             JSObject** pobj, nanojit::LIns** pobj_ins,
                                             VMSideExit* exit);
    JS_REQUIRES_STACK RecordingStatus guardPrototypeHasNoIndexedProperties(JSObject* obj,
                                                                             nanojit::LIns* obj_ins,
                                                                             ExitType exitType);
    JS_REQUIRES_STACK RecordingStatus guardNotGlobalObject(JSObject* obj,
                                                             nanojit::LIns* obj_ins);
    void clearFrameSlotsFromCache();
    JS_REQUIRES_STACK void putArguments();
    JS_REQUIRES_STACK RecordingStatus guardCallee(jsval& callee);
    JS_REQUIRES_STACK JSStackFrame      *guardArguments(JSObject *obj, nanojit::LIns* obj_ins,
                                                        unsigned *depthp);
    JS_REQUIRES_STACK RecordingStatus getClassPrototype(JSObject* ctor,
                                                          nanojit::LIns*& proto_ins);
    JS_REQUIRES_STACK RecordingStatus getClassPrototype(JSProtoKey key,
                                                          nanojit::LIns*& proto_ins);
    JS_REQUIRES_STACK RecordingStatus newArray(JSObject* ctor, uint32 argc, jsval* argv,
                                                 jsval* rval);
    JS_REQUIRES_STACK RecordingStatus newString(JSObject* ctor, uint32 argc, jsval* argv,
                                                  jsval* rval);
    JS_REQUIRES_STACK RecordingStatus interpretedFunctionCall(jsval& fval, JSFunction* fun,
                                                                uintN argc, bool constructing);
    JS_REQUIRES_STACK void propagateFailureToBuiltinStatus(nanojit::LIns *ok_ins,
                                                           nanojit::LIns *&status_ins);
    JS_REQUIRES_STACK RecordingStatus emitNativeCall(JSSpecializedNative* sn, uintN argc,
                                                       nanojit::LIns* args[], bool rooted);
    JS_REQUIRES_STACK void emitNativePropertyOp(JSScope* scope,
                                                JSScopeProperty* sprop,
                                                nanojit::LIns* obj_ins,
                                                bool setflag,
                                                nanojit::LIns* boxed_ins);
    JS_REQUIRES_STACK RecordingStatus callSpecializedNative(JSNativeTraceInfo* trcinfo, uintN argc,
                                                              bool constructing);
    JS_REQUIRES_STACK RecordingStatus callNative(uintN argc, JSOp mode);
    JS_REQUIRES_STACK RecordingStatus functionCall(uintN argc, JSOp mode);

    JS_REQUIRES_STACK void trackCfgMerges(jsbytecode* pc);
    JS_REQUIRES_STACK void emitIf(jsbytecode* pc, bool cond, nanojit::LIns* x);
    JS_REQUIRES_STACK void fuseIf(jsbytecode* pc, bool cond, nanojit::LIns* x);
    JS_REQUIRES_STACK AbortableRecordingStatus checkTraceEnd(jsbytecode* pc);

    bool hasMethod(JSObject* obj, jsid id);
    JS_REQUIRES_STACK bool hasIteratorMethod(JSObject* obj);

    JS_REQUIRES_STACK jsatomid getFullIndex(ptrdiff_t pcoff = 0);

public:

    inline void*
    operator new(size_t size)
    {
        return calloc(1, size);
    }

    inline void
    operator delete(void *p)
    {
        free(p);
    }

    JS_REQUIRES_STACK
    TraceRecorder(JSContext* cx, VMSideExit*, nanojit::Fragment*, TreeInfo*,
                  unsigned stackSlots, unsigned ngslots, JSTraceType* typeMap,
                  VMSideExit* expectedInnerExit, jsbytecode* outerTree,
                  uint32 outerArgc, MonitorReason monitorReason);
    ~TraceRecorder();

    bool outOfMemory();

    static JS_REQUIRES_STACK AbortableRecordingStatus monitorRecording(JSContext* cx, TraceRecorder* tr,
                                                                       JSOp op);

    JS_REQUIRES_STACK JSTraceType determineSlotType(jsval* vp);

    /*
     * Examines current interpreter state to record information suitable for
     * returning to the interpreter through a side exit of the given type.
     */
    JS_REQUIRES_STACK VMSideExit* snapshot(ExitType exitType);

    /*
     * Creates a separate but identical copy of the given side exit, allowing
     * the guards associated with each to be entirely separate even after
     * subsequent patching.
     */
    JS_REQUIRES_STACK VMSideExit* copy(VMSideExit* exit);

    /*
     * Creates an instruction whose payload is a GuardRecord for the given exit.
     * The instruction is suitable for use as the final argument of a single
     * call to LirBuffer::insGuard; do not reuse the returned value.
     */
    JS_REQUIRES_STACK nanojit::GuardRecord* createGuardRecord(VMSideExit* exit);

    nanojit::Fragment* getFragment() const { return fragment; }
    TreeInfo* getTreeInfo() const { return treeInfo; }
    JS_REQUIRES_STACK AbortableRecordingStatus compile(JSTraceMonitor* tm);
    JS_REQUIRES_STACK AbortableRecordingStatus closeLoop();
    JS_REQUIRES_STACK AbortableRecordingStatus closeLoop(VMSideExit* exit);
    JS_REQUIRES_STACK AbortableRecordingStatus closeLoop(SlotMap& slotMap, VMSideExit* exit);
    JS_REQUIRES_STACK AbortableRecordingStatus endLoop();
    JS_REQUIRES_STACK AbortableRecordingStatus endLoop(VMSideExit* exit);
    JS_REQUIRES_STACK void joinEdgesToEntry(VMFragment* peer_root);
    JS_REQUIRES_STACK void adjustCallerTypes(nanojit::Fragment* f);
    JS_REQUIRES_STACK void prepareTreeCall(VMFragment* inner);
    JS_REQUIRES_STACK void emitTreeCall(VMFragment* inner, VMSideExit* exit);
    JS_REQUIRES_STACK VMFragment* findNestedCompatiblePeer(VMFragment* f);
    JS_REQUIRES_STACK AbortableRecordingStatus attemptTreeCall(VMFragment* inner,
                                                               uintN& inlineCallCount);
    unsigned getCallDepth() const;

    JS_REQUIRES_STACK void determineGlobalTypes(JSTraceType* typeMap);
    nanojit::LIns* demoteIns(nanojit::LIns* ins);

    JS_REQUIRES_STACK VMSideExit* downSnapshot(FrameInfo* downFrame);
    JS_REQUIRES_STACK AbortableRecordingStatus upRecursion();
    JS_REQUIRES_STACK AbortableRecordingStatus downRecursion();
    JS_REQUIRES_STACK AbortableRecordingStatus record_EnterFrame(uintN& inlineCallCount);
    JS_REQUIRES_STACK AbortableRecordingStatus record_LeaveFrame();
    JS_REQUIRES_STACK AbortableRecordingStatus record_SetPropHit(JSPropCacheEntry* entry,
                                                                  JSScopeProperty* sprop);
    JS_REQUIRES_STACK AbortableRecordingStatus record_DefLocalFunSetSlot(uint32 slot, JSObject* obj);
    JS_REQUIRES_STACK AbortableRecordingStatus record_NativeCallComplete();

    void forgetGuardedShapesForObject(JSObject* obj);

#ifdef DEBUG
    JS_REQUIRES_STACK void tprint(const char *format, int count, nanojit::LIns *insa[]);
    JS_REQUIRES_STACK void tprint(const char *format);
    JS_REQUIRES_STACK void tprint(const char *format, nanojit::LIns *ins);
    JS_REQUIRES_STACK void tprint(const char *format, nanojit::LIns *ins1,
                                  nanojit::LIns *ins2);
    JS_REQUIRES_STACK void tprint(const char *format, nanojit::LIns *ins1,
                                  nanojit::LIns *ins2, nanojit::LIns *ins3);
    JS_REQUIRES_STACK void tprint(const char *format, nanojit::LIns *ins1,
                                  nanojit::LIns *ins2, nanojit::LIns *ins3,
                                  nanojit::LIns *ins4);
    JS_REQUIRES_STACK void tprint(const char *format, nanojit::LIns *ins1,
                                  nanojit::LIns *ins2, nanojit::LIns *ins3,
                                  nanojit::LIns *ins4, nanojit::LIns *ins5);
    JS_REQUIRES_STACK void tprint(const char *format, nanojit::LIns *ins1,
                                  nanojit::LIns *ins2, nanojit::LIns *ins3,
                                  nanojit::LIns *ins4, nanojit::LIns *ins5,
                                  nanojit::LIns *ins6);
#endif

#define OPDEF(op,val,name,token,length,nuses,ndefs,prec,format)               \
    JS_REQUIRES_STACK AbortableRecordingStatus record_##op();
# include "jsopcode.tbl"
#undef OPDEF

    friend class ImportBoxedStackSlotVisitor;
    friend class ImportUnboxedStackSlotVisitor;
    friend class ImportGlobalSlotVisitor;
    friend class AdjustCallerGlobalTypesVisitor;
    friend class AdjustCallerStackTypesVisitor;
    friend class TypeCompatibilityVisitor;
    friend class SlotMap;
    friend class DefaultSlotMap;
    friend class RecursiveSlotMap;
    friend jsval *js_ConcatPostImacroStackCleanup(uint32 argc, JSFrameRegs &regs,
                                                  TraceRecorder *recorder);
};
#define TRACING_ENABLED(cx)       JS_HAS_OPTION(cx, JSOPTION_JIT)
#define TRACE_RECORDER(cx)        (JS_TRACE_MONITOR(cx).recorder)
#define SET_TRACE_RECORDER(cx,tr) (JS_TRACE_MONITOR(cx).recorder = (tr))

#define JSOP_IN_RANGE(op,lo,hi)   (uintN((op) - (lo)) <= uintN((hi) - (lo)))
#define JSOP_IS_BINARY(op)        JSOP_IN_RANGE(op, JSOP_BITOR, JSOP_MOD)
#define JSOP_IS_UNARY(op)         JSOP_IN_RANGE(op, JSOP_NEG, JSOP_POS)
#define JSOP_IS_EQUALITY(op)      JSOP_IN_RANGE(op, JSOP_EQ, JSOP_NE)

#define TRACE_ARGS_(x,args)                                                   \
    JS_BEGIN_MACRO                                                            \
        if (TraceRecorder* tr_ = TRACE_RECORDER(cx)) {                        \
            AbortableRecordingStatus status = tr_->record_##x args;           \
            if (StatusAbortsRecording(status)) {                              \
                if (TRACE_RECORDER(cx))                                       \
                    js_AbortRecording(cx, #x);                                \
                if (status == ARECORD_ERROR)                                  \
                    goto error;                                               \
            }                                                                 \
            JS_ASSERT(status != ARECORD_IMACRO);                              \
        }                                                                     \
    JS_END_MACRO

#define TRACE_ARGS(x,args)      TRACE_ARGS_(x, args)
#define TRACE_0(x)              TRACE_ARGS(x, ())
#define TRACE_1(x,a)            TRACE_ARGS(x, (a))
#define TRACE_2(x,a,b)          TRACE_ARGS(x, (a, b))

extern JS_REQUIRES_STACK bool
js_MonitorLoopEdge(JSContext* cx, uintN& inlineCallCount, MonitorReason reason);

#ifdef DEBUG
# define js_AbortRecording(cx, reason) js_AbortRecordingImpl(cx, reason)
#else
# define js_AbortRecording(cx, reason) js_AbortRecordingImpl(cx)
#endif

extern JS_REQUIRES_STACK void
js_AbortRecording(JSContext* cx, const char* reason);

extern void
js_InitJIT(JSTraceMonitor *tm);

extern void
js_FinishJIT(JSTraceMonitor *tm);

extern void
js_PurgeScriptFragments(JSContext* cx, JSScript* script);

extern bool
js_OverfullJITCache(JSTraceMonitor* tm);

extern void
js_ResetJIT(JSContext* cx);

extern void
js_PurgeJITOracle();

extern JSObject *
js_GetBuiltinFunction(JSContext *cx, uintN index);

extern void
js_SetMaxCodeCacheBytes(JSContext* cx, uint32 bytes);

extern bool
js_NativeToValue(JSContext* cx, jsval& v, JSTraceType type, double* slot);

#ifdef MOZ_TRACEVIS

extern JS_FRIEND_API(bool)
JS_StartTraceVis(const char* filename);

extern JS_FRIEND_API(JSBool)
js_StartTraceVis(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
                 jsval *rval);

extern JS_FRIEND_API(bool)
JS_StopTraceVis();

extern JS_FRIEND_API(JSBool)
js_StopTraceVis(JSContext *cx, JSObject *obj, uintN argc, jsval *argv,
                jsval *rval);

/* Must contain no more than 16 items. */
enum TraceVisState {
    // Special: means we returned from current activity to last
    S_EXITLAST,
    // Activities
    S_INTERP,
    S_MONITOR,
    S_RECORD,
    S_COMPILE,
    S_EXECUTE,
    S_NATIVE,
    // Events: these all have (bit 3) == 1.
    S_RESET = 8
};

/* Reason for an exit to the interpreter. */
enum TraceVisExitReason {
    R_NONE,
    R_ABORT,
    /* Reasons in js_MonitorLoopEdge */
    R_INNER_SIDE_EXIT,
    R_DOUBLES,
    R_CALLBACK_PENDING,
    R_OOM_GETANCHOR,
    R_BACKED_OFF,
    R_COLD,
    R_FAIL_RECORD_TREE,
    R_MAX_PEERS,
    R_FAIL_EXECUTE_TREE,
    R_FAIL_STABILIZE,
    R_FAIL_EXTEND_FLUSH,
    R_FAIL_EXTEND_MAX_BRANCHES,
    R_FAIL_EXTEND_START,
    R_FAIL_EXTEND_COLD,
    R_NO_EXTEND_OUTER,
    R_MISMATCH_EXIT,
    R_OOM_EXIT,
    R_TIMEOUT_EXIT,
    R_DEEP_BAIL_EXIT,
    R_STATUS_EXIT,
    R_OTHER_EXIT
};

enum TraceVisFlushReason {
    FR_DEEP_BAIL,
    FR_OOM,
    FR_GLOBAL_SHAPE_MISMATCH,
    FR_GLOBALS_FULL
};

const unsigned long long MS64_MASK = 0xfull << 60;
const unsigned long long MR64_MASK = 0x1full << 55;
const unsigned long long MT64_MASK = ~(MS64_MASK | MR64_MASK);

extern FILE* traceVisLogFile;
extern JSHashTable *traceVisScriptTable;

extern JS_FRIEND_API(void)
js_StoreTraceVisState(JSContext *cx, TraceVisState s, TraceVisExitReason r);

static inline void
js_LogTraceVisState(JSContext *cx, TraceVisState s, TraceVisExitReason r)
{
    if (traceVisLogFile) {
        unsigned long long sllu = s;
        unsigned long long rllu = r;
        unsigned long long d = (sllu << 60) | (rllu << 55) | (rdtsc() & MT64_MASK);
        fwrite(&d, sizeof(d), 1, traceVisLogFile);
    }
    if (traceVisScriptTable) {
        js_StoreTraceVisState(cx, s, r);
    }
}

/*
 * Although this runs the same code as js_LogTraceVisState, it is a separate
 * function because the meaning of the log entry is different. Also, the entry
 * formats may diverge someday.
 */
static inline void
js_LogTraceVisEvent(JSContext *cx, TraceVisState s, TraceVisFlushReason r)
{
    js_LogTraceVisState(cx, s, (TraceVisExitReason) r);
}

static inline void
js_EnterTraceVisState(JSContext *cx, TraceVisState s, TraceVisExitReason r)
{
    js_LogTraceVisState(cx, s, r);
}

static inline void
js_ExitTraceVisState(JSContext *cx, TraceVisExitReason r)
{
    js_LogTraceVisState(cx, S_EXITLAST, r);
}

struct TraceVisStateObj {
    TraceVisExitReason r;
    JSContext *mCx;

    inline TraceVisStateObj(JSContext *cx, TraceVisState s) : r(R_NONE)
    {
        js_EnterTraceVisState(cx, s, R_NONE);
        mCx = cx;
    }
    inline ~TraceVisStateObj()
    {
        js_ExitTraceVisState(mCx, r);
    }
};

#endif /* MOZ_TRACEVIS */

extern jsval *
js_ConcatPostImacroStackCleanup(uint32 argc, JSFrameRegs &regs,
                                TraceRecorder *recorder);

#else  /* !JS_TRACER */

#define TRACE_0(x)              ((void)0)
#define TRACE_1(x,a)            ((void)0)
#define TRACE_2(x,a,b)          ((void)0)

#endif /* !JS_TRACER */

#endif /* jstracer_h___ */
