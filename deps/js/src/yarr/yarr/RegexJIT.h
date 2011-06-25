/*
 * Copyright (C) 2009 Apple Inc. All rights reserved.
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

#ifndef RegexJIT_h
#define RegexJIT_h

#if ENABLE_ASSEMBLER

#include "assembler/assembler/MacroAssembler.h"
#include "assembler/assembler/MacroAssemblerCodeRef.h"
#include "assembler/jit/ExecutableAllocator.h"
#include "RegexPattern.h"
#include "yarr/jswtfbridge.h"

#include "yarr/pcre/pcre.h"
struct JSRegExp; // temporary, remove when fallback is removed.

#if WTF_CPU_X86 && !WTF_COMPILER_MSVC && !WTF_COMPILER_SUNPRO
#define YARR_CALL __attribute__ ((regparm (3)))
#else
#define YARR_CALL
#endif

struct JSContext;

namespace JSC {

namespace Yarr {

class RegexCodeBlock {
    typedef int (*RegexJITCode)(const UChar* input, unsigned start, unsigned length, int* output) YARR_CALL;

public:
    RegexCodeBlock()
        : m_fallback(0)
    {
    }

    ~RegexCodeBlock()
    {
        if (m_fallback)
            jsRegExpFree(m_fallback);
        if (m_ref.m_size)
            m_ref.m_executablePool->release();
    }

    JSRegExp* getFallback() { return m_fallback; }
    void setFallback(JSRegExp* fallback) { m_fallback = fallback; }

    bool operator!() { return (!m_ref.m_code.executableAddress() && !m_fallback); }
    void set(MacroAssembler::CodeRef ref) { m_ref = ref; }

    int execute(const UChar* input, unsigned start, unsigned length, int* output)
    {
        void *code = m_ref.m_code.executableAddress();
        return JS_EXTENSION((reinterpret_cast<RegexJITCode>(code))(input, start, length, output));
    }

private:
    MacroAssembler::CodeRef m_ref;
    JSRegExp* m_fallback;
};

void jitCompileRegex(ExecutableAllocator &allocator, RegexCodeBlock& jitObject, const UString& pattern, unsigned& numSubpatterns, int& error, bool &fellBack, bool ignoreCase = false, bool multiline = false
#ifdef ANDROID
                     , bool forceFallback = false
#endif
);

inline int executeRegex(JSContext *cx, RegexCodeBlock& jitObject, const UChar* input, unsigned start, unsigned length, int* output, int outputArraySize)
{
    if (JSRegExp* fallback = jitObject.getFallback()) {
        int result = jsRegExpExecute(cx, fallback, input, length, start, output, outputArraySize);

        if (result == JSRegExpErrorHitLimit)
            return HitRecursionLimit;

        // -1 represents no-match for both PCRE and YARR.
        JS_ASSERT(result >= -1);
        return result;
    }

    return jitObject.execute(input, start, length, output);
}

} } // namespace JSC::Yarr

#endif /* ENABLE_ASSEMBLER */

#endif // RegexJIT_h
