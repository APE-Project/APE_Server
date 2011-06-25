/*
 * Copyright (C) 2008 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#ifndef ExecutableAllocator_h
#define ExecutableAllocator_h

#include <stddef.h> // for ptrdiff_t
#include <limits>
#include "assembler/wtf/Assertions.h"

#include "jsapi.h"
#include "jsprvtd.h"
#include "jsvector.h"
#include "jslock.h"

#if WTF_PLATFORM_IPHONE
#include <libkern/OSCacheControl.h>
#include <sys/mman.h>
#endif

#if WTF_PLATFORM_SYMBIAN
#include <e32std.h>
#endif

#if WTF_CPU_MIPS && WTF_PLATFORM_LINUX
#include <sys/cachectl.h>
#endif

#if WTF_PLATFORM_WINCE
// From pkfuncs.h (private header file from the Platform Builder)
#define CACHE_SYNC_ALL 0x07F
extern "C" __declspec(dllimport) void CacheRangeFlush(LPVOID pAddr, DWORD dwLength, DWORD dwFlags);
#endif

#define JIT_ALLOCATOR_PAGE_SIZE (ExecutableAllocator::pageSize)
/*
 * On Windows, VirtualAlloc effectively allocates in 64K chunks. (Technically,
 * it allocates in page chunks, but the starting address is always a multiple
 * of 64K, so each allocation uses up 64K of address space.)  So a size less
 * than that would be pointless.  But it turns out that 64KB is a reasonable
 * size for all platforms.
 */
#define JIT_ALLOCATOR_LARGE_ALLOC_SIZE (ExecutableAllocator::pageSize * 16)

#if ENABLE_ASSEMBLER_WX_EXCLUSIVE
#define PROTECTION_FLAGS_RW (PROT_READ | PROT_WRITE)
#define PROTECTION_FLAGS_RX (PROT_READ | PROT_EXEC)
#define INITIAL_PROTECTION_FLAGS PROTECTION_FLAGS_RX
#else
#define INITIAL_PROTECTION_FLAGS (PROT_READ | PROT_WRITE | PROT_EXEC)
#endif

namespace JSC {

// Something included via windows.h defines a macro with this name,
// which causes the function below to fail to compile.
#ifdef _MSC_VER
# undef max
#endif

const size_t OVERSIZE_ALLOCATION = size_t(-1);

inline size_t roundUpAllocationSize(size_t request, size_t granularity)
{
    if ((std::numeric_limits<size_t>::max() - granularity) <= request)
        return OVERSIZE_ALLOCATION;
    
    // Round up to next page boundary
    size_t size = request + (granularity - 1);
    size = size & ~(granularity - 1);
    JS_ASSERT(size >= request);
    return size;
}

}

#if ENABLE_ASSEMBLER

//#define DEBUG_STRESS_JSC_ALLOCATOR

namespace JSC {

  // These are reference-counted. A new one (from the constructor or create)
  // starts with a count of 1. 
  class ExecutablePool {
private:
    struct Allocation {
        char* pages;
        size_t size;
#if WTF_PLATFORM_SYMBIAN
        RChunk* chunk;
#endif
    };
    typedef js::Vector<Allocation, 2, js::SystemAllocPolicy> AllocationList;

    // Reference count for automatic reclamation.
    unsigned m_refCount;

public:
    // It should be impossible for us to roll over, because only small
    // pools have multiple holders, and they have one holder per chunk
    // of generated code, and they only hold 16KB or so of code.
    void addRef()
    {
        JS_ASSERT(m_refCount);
        ++m_refCount;
    }

    void release()
    { 
        JS_ASSERT(m_refCount != 0);
        if (--m_refCount == 0)
            js_delete(this);
    }

    static ExecutablePool* create(size_t n)
    {
        /* We can't (easily) use js_new() here because the constructor is private. */
        void *memory = js_malloc(sizeof(ExecutablePool));
        ExecutablePool *pool = memory ? new(memory) ExecutablePool(n) : NULL;
        if (!pool || !pool->m_freePtr) {
            js_delete(pool);
            return NULL;
        }
        return pool;
    }

    void* alloc(size_t n)
    {
        JS_ASSERT(m_freePtr <= m_end);

        // Round 'n' up to a multiple of word size; if all allocations are of
        // word sized quantities, then all subsequent allocations will be aligned.
        n = roundUpAllocationSize(n, sizeof(void*));
        if (n == OVERSIZE_ALLOCATION)
            return NULL;

        if (static_cast<ptrdiff_t>(n) < (m_end - m_freePtr)) {
            void* result = m_freePtr;
            m_freePtr += n;
            return result;
        }

        // Insufficient space to allocate in the existing pool
        // so we need allocate into a new pool
        return poolAllocate(n);
    }
    
    ~ExecutablePool()
    {
        Allocation* end = m_pools.end();
        for (Allocation* ptr = m_pools.begin(); ptr != end; ++ptr)
            ExecutablePool::systemRelease(*ptr);
    }

    size_t available() const { return (m_pools.length() > 1) ? 0 : m_end - m_freePtr; }

    // Flag for downstream use, whether to try to release references to this pool.
    bool m_destroy;

    // GC number in which the m_destroy flag was most recently set. Used downstream to
    // remember whether m_destroy was computed for the currently active GC.
    size_t m_gcNumber;

private:
    // On OOM, this will return an Allocation where pages is NULL.
    static Allocation systemAlloc(size_t n);
    static void systemRelease(const Allocation& alloc);

    ExecutablePool(size_t n);

    void* poolAllocate(size_t n);

    char* m_freePtr;
    char* m_end;
    AllocationList m_pools;
};

class ExecutableAllocator {
    enum ProtectionSeting { Writable, Executable };

    // Initialization can fail so we use a create method instead.
    ExecutableAllocator() {}
public:
    static size_t pageSize;

    // Returns NULL on OOM.
    static ExecutableAllocator *create()
    {
        /* We can't (easily) use js_new() here because the constructor is private. */
        void *memory = js_malloc(sizeof(ExecutableAllocator));
        ExecutableAllocator *allocator = memory ? new(memory) ExecutableAllocator() : NULL;
        if (!allocator)
            return allocator;

        if (!pageSize)
            intializePageSize();
        ExecutablePool *pool = ExecutablePool::create(JIT_ALLOCATOR_LARGE_ALLOC_SIZE);
        if (!pool) {
            js_delete(allocator);
            return NULL;
        }
        JS_ASSERT(allocator->m_smallAllocationPools.empty());
        allocator->m_smallAllocationPools.append(pool);
        return allocator;
    }

    ~ExecutableAllocator()
    {
        for (size_t i = 0; i < m_smallAllocationPools.length(); i++)
            js_delete(m_smallAllocationPools[i]);
    }

    // poolForSize returns reference-counted objects. The caller owns a reference
    // to the object; i.e., poolForSize increments the count before returning the
    // object.

    ExecutablePool* poolForSize(size_t n)
    {
#ifndef DEBUG_STRESS_JSC_ALLOCATOR
        // Try to fit in an existing small allocator.  Use the pool with the
        // least available space that is big enough (best-fit).  This is the
        // best strategy because (a) it maximizes the chance of the next
        // allocation fitting in a small pool, and (b) it minimizes the
        // potential waste when a small pool is next abandoned.
        ExecutablePool *minPool = NULL;
        for (size_t i = 0; i < m_smallAllocationPools.length(); i++) {
            ExecutablePool *pool = m_smallAllocationPools[i];
            if (n <= pool->available() && (!minPool || pool->available() < minPool->available()))
                minPool = pool;
        }
        if (minPool) {
            minPool->addRef();
            return minPool;
        }
#endif

        // If the request is large, we just provide a unshared allocator
        if (n > JIT_ALLOCATOR_LARGE_ALLOC_SIZE)
            return ExecutablePool::create(n);

        // Create a new allocator
        ExecutablePool* pool = ExecutablePool::create(JIT_ALLOCATOR_LARGE_ALLOC_SIZE);
        if (!pool)
            return NULL;
  	    // At this point, local |pool| is the owner.

        if (m_smallAllocationPools.length() < maxSmallPools) {
            // We haven't hit the maximum number of live pools;  add the new pool.
            m_smallAllocationPools.append(pool);
            pool->addRef();
        } else {
            // Find the pool with the least space.
            int iMin = 0;
            for (size_t i = 1; i < m_smallAllocationPools.length(); i++)
                if (m_smallAllocationPools[i]->available() <
                    m_smallAllocationPools[iMin]->available())
                {
                    iMin = i;
                }

            // If the new allocator will result in more free space than the small
            // pool with the least space, then we will use it instead
            ExecutablePool *minPool = m_smallAllocationPools[iMin];
            if ((pool->available() - n) > minPool->available()) {
                minPool->release();
                m_smallAllocationPools[iMin] = pool;
                pool->addRef();
            }
        }

   	    // Pass ownership to the caller.
        return pool;
    }

#if ENABLE_ASSEMBLER_WX_EXCLUSIVE
    static void makeWritable(void* start, size_t size)
    {
        reprotectRegion(start, size, Writable);
    }

    static void makeExecutable(void* start, size_t size)
    {
        reprotectRegion(start, size, Executable);
    }
#else
    static void makeWritable(void*, size_t) {}
    static void makeExecutable(void*, size_t) {}
#endif


#if WTF_CPU_X86 || WTF_CPU_X86_64
    static void cacheFlush(void*, size_t)
    {
    }
#elif WTF_CPU_MIPS
    static void cacheFlush(void* code, size_t size)
    {
#if WTF_COMPILER_GCC && (GCC_VERSION >= 40300)
#if WTF_MIPS_ISA_REV(2) && (GCC_VERSION < 40403)
        int lineSize;
        asm("rdhwr %0, $1" : "=r" (lineSize));
        //
        // Modify "start" and "end" to avoid GCC 4.3.0-4.4.2 bug in
        // mips_expand_synci_loop that may execute synci one more time.
        // "start" points to the fisrt byte of the cache line.
        // "end" points to the last byte of the line before the last cache line.
        // Because size is always a multiple of 4, this is safe to set
        // "end" to the last byte.
        //
        intptr_t start = reinterpret_cast<intptr_t>(code) & (-lineSize);
        intptr_t end = ((reinterpret_cast<intptr_t>(code) + size - 1) & (-lineSize)) - 1;
        __builtin___clear_cache(reinterpret_cast<char*>(start), reinterpret_cast<char*>(end));
#else
        intptr_t end = reinterpret_cast<intptr_t>(code) + size;
        __builtin___clear_cache(reinterpret_cast<char*>(code), reinterpret_cast<char*>(end));
#endif
#else
        _flush_cache(reinterpret_cast<char*>(code), size, BCACHE);
#endif
    }
#elif WTF_CPU_ARM_THUMB2 && WTF_PLATFORM_IPHONE
    static void cacheFlush(void* code, size_t size)
    {
        sys_dcache_flush(code, size);
        sys_icache_invalidate(code, size);
    }
#elif WTF_CPU_ARM_THUMB2 && WTF_PLATFORM_LINUX
    static void cacheFlush(void* code, size_t size)
    {
        asm volatile (
            "push    {r7}\n"
            "mov     r0, %0\n"
            "mov     r1, %1\n"
            "movw    r7, #0x2\n"
            "movt    r7, #0xf\n"
            "movs    r2, #0x0\n"
            "svc     0x0\n"
            "pop     {r7}\n"
            :
            : "r" (code), "r" (reinterpret_cast<char*>(code) + size)
            : "r0", "r1", "r2");
    }
#elif WTF_PLATFORM_SYMBIAN
    static void cacheFlush(void* code, size_t size)
    {
        User::IMB_Range(code, static_cast<char*>(code) + size);
    }
#elif WTF_CPU_ARM_TRADITIONAL && WTF_PLATFORM_LINUX && WTF_COMPILER_RVCT
    static __asm void cacheFlush(void* code, size_t size);
#elif WTF_CPU_ARM_TRADITIONAL && (WTF_PLATFORM_LINUX || WTF_PLATFORM_ANDROID) && WTF_COMPILER_GCC
    static void cacheFlush(void* code, size_t size)
    {
        asm volatile (
            "push    {r7}\n"
            "mov     r0, %0\n"
            "mov     r1, %1\n"
            "mov     r7, #0xf0000\n"
            "add     r7, r7, #0x2\n"
            "mov     r2, #0x0\n"
            "svc     0x0\n"
            "pop     {r7}\n"
            :
            : "r" (code), "r" (reinterpret_cast<char*>(code) + size)
            : "r0", "r1", "r2");
    }
#elif WTF_PLATFORM_WINCE
    static void cacheFlush(void* code, size_t size)
    {
        CacheRangeFlush(code, size, CACHE_SYNC_ALL);
    }
#else
    #error "The cacheFlush support is missing on this platform."
#endif

private:

#if ENABLE_ASSEMBLER_WX_EXCLUSIVE
    static void reprotectRegion(void*, size_t, ProtectionSeting);
#endif

    static const size_t maxSmallPools = 4;
    typedef js::Vector<ExecutablePool *, maxSmallPools, js::SystemAllocPolicy > SmallExecPoolVector;
    SmallExecPoolVector m_smallAllocationPools;
    static void intializePageSize();
};

// This constructor can fail due to OOM. If it does, m_freePtr will be
// set to NULL. 
inline ExecutablePool::ExecutablePool(size_t n) : m_refCount(1), m_destroy(false), m_gcNumber(0)
{
    size_t allocSize = roundUpAllocationSize(n, JIT_ALLOCATOR_PAGE_SIZE);
    if (allocSize == OVERSIZE_ALLOCATION) {
        m_freePtr = NULL;
        return;
    }
#ifdef DEBUG_STRESS_JSC_ALLOCATOR
    Allocation mem = systemAlloc(size_t(4294967291));
#else
    Allocation mem = systemAlloc(allocSize);
#endif
    if (!mem.pages) {
        m_freePtr = NULL;
        return;
    }
    if (!m_pools.append(mem)) {
        systemRelease(mem);
        m_freePtr = NULL;
        return;
    }
    m_freePtr = mem.pages;
    m_end = m_freePtr + allocSize;
}

inline void* ExecutablePool::poolAllocate(size_t n)
{
    size_t allocSize = roundUpAllocationSize(n, JIT_ALLOCATOR_PAGE_SIZE);
    if (allocSize == OVERSIZE_ALLOCATION)
        return NULL;
    
#ifdef DEBUG_STRESS_JSC_ALLOCATOR
    Allocation result = systemAlloc(size_t(4294967291));
#else
    Allocation result = systemAlloc(allocSize);
#endif
    if (!result.pages)
        return NULL;
    
    JS_ASSERT(m_end >= m_freePtr);
    if ((allocSize - n) > static_cast<size_t>(m_end - m_freePtr)) {
        // Replace allocation pool
        m_freePtr = result.pages + n;
        m_end = result.pages + allocSize;
    }

    m_pools.append(result);
    return result.pages;
}

}

#endif // ENABLE(ASSEMBLER)

#endif // !defined(ExecutableAllocator)
