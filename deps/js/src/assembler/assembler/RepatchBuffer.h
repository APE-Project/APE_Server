/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=79:
 *
 * ***** BEGIN LICENSE BLOCK *****
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
 * 
 * ***** END LICENSE BLOCK ***** */

#ifndef RepatchBuffer_h
#define RepatchBuffer_h

#include "assembler/wtf/Platform.h"

#if ENABLE_ASSEMBLER

#include <assembler/MacroAssembler.h>
#include <moco/MocoStubs.h> //MOCO

namespace JSC {

// RepatchBuffer:
//
// This class is used to modify code after code generation has been completed,
// and after the code has potentially already been executed.  This mechanism is
// used to apply optimizations to the code.
//
class RepatchBuffer {
    typedef MacroAssemblerCodePtr CodePtr;

public:
    RepatchBuffer(const MacroAssemblerCodeRef &ref)
    {
        m_start = ref.m_code.executableAddress();
        m_size = ref.m_size;
        mprot = true;

        if (mprot)
            ExecutableAllocator::makeWritable(m_start, m_size);
    }

    RepatchBuffer(const JITCode &code)
    {
        m_start = code.start();
        m_size = code.size();
        mprot = true;

        if (mprot)
            ExecutableAllocator::makeWritable(m_start, m_size);
    }

    ~RepatchBuffer()
    {
        if (mprot)
            ExecutableAllocator::makeExecutable(m_start, m_size);
    }

    void relink(CodeLocationJump jump, CodeLocationLabel destination)
    {
        MacroAssembler::repatchJump(jump, destination);
    }

    bool canRelink(CodeLocationJump jump, CodeLocationLabel destination)
    {
        return MacroAssembler::canRepatchJump(jump, destination);
    }

    void relink(CodeLocationCall call, CodeLocationLabel destination)
    {
        MacroAssembler::repatchCall(call, destination);
    }

    void relink(CodeLocationCall call, FunctionPtr destination)
    {
        MacroAssembler::repatchCall(call, destination);
    }

    void relink(CodeLocationNearCall nearCall, CodePtr destination)
    {
        MacroAssembler::repatchNearCall(nearCall, CodeLocationLabel(destination));
    }

    void relink(CodeLocationNearCall nearCall, CodeLocationLabel destination)
    {
        MacroAssembler::repatchNearCall(nearCall, destination);
    }

    void repatch(CodeLocationDataLabel32 dataLabel32, int32_t value)
    {
        MacroAssembler::repatchInt32(dataLabel32, value);
    }

    void repatch(CodeLocationDataLabelPtr dataLabelPtr, void* value)
    {
        MacroAssembler::repatchPointer(dataLabelPtr, value);
    }

    void repatchLoadPtrToLEA(CodeLocationInstruction instruction)
    {
        MacroAssembler::repatchLoadPtrToLEA(instruction);
    }

    void repatchLEAToLoadPtr(CodeLocationInstruction instruction)
    {
        MacroAssembler::repatchLEAToLoadPtr(instruction);
    }

    void relinkCallerToTrampoline(ReturnAddressPtr returnAddress, CodeLocationLabel label)
    {
        relink(CodeLocationCall(CodePtr(returnAddress)), label);
    }
    
    void relinkCallerToTrampoline(ReturnAddressPtr returnAddress, CodePtr newCalleeFunction)
    {
        relinkCallerToTrampoline(returnAddress, CodeLocationLabel(newCalleeFunction));
    }

    void relinkCallerToFunction(ReturnAddressPtr returnAddress, FunctionPtr function)
    {
        relink(CodeLocationCall(CodePtr(returnAddress)), function);
    }
    
    void relinkNearCallerToTrampoline(ReturnAddressPtr returnAddress, CodeLocationLabel label)
    {
        relink(CodeLocationNearCall(CodePtr(returnAddress)), label);
    }
    
    void relinkNearCallerToTrampoline(ReturnAddressPtr returnAddress, CodePtr newCalleeFunction)
    {
        relinkNearCallerToTrampoline(returnAddress, CodeLocationLabel(newCalleeFunction));
    }

protected:
    void* m_start;
    size_t m_size;
    bool mprot;
};

} // namespace JSC

#endif // ENABLE(ASSEMBLER)

#endif // RepatchBuffer_h
