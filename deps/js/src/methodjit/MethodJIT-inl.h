/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
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
 *   David Anderson <danderson@mozilla.com>
 *   David Mandelin <dmandelin@mozilla.com>
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

#if !defined jsjaeger_methodjit_inl_h__ && defined JS_METHODJIT
#define jsjaeger_methodjit_inl_h__

namespace js {
namespace mjit {

enum CompileRequest
{
    CompileRequest_Interpreter,
    CompileRequest_JIT
};

/* Number of times a script must be called before we run it in the methodjit. */
static const size_t CALLS_BEFORE_COMPILE = 16;

/* Number of loop back-edges we execute in the interpreter before methodjitting. */
static const size_t BACKEDGES_BEFORE_COMPILE = 16;

static inline CompileStatus
CanMethodJIT(JSContext *cx, JSScript *script, JSStackFrame *fp, CompileRequest request)
{
    if (!cx->methodJitEnabled)
        return Compile_Abort;
    JITScriptStatus status = script->getJITStatus(fp->isConstructing());
    if (status == JITScript_Invalid)
        return Compile_Abort;
    if (request == CompileRequest_Interpreter &&
        status == JITScript_None &&
        !cx->hasRunOption(JSOPTION_METHODJIT_ALWAYS) &&
        script->incCallCount() <= CALLS_BEFORE_COMPILE)
    {
        return Compile_Skipped;
    }
    if (status == JITScript_None)
        return TryCompile(cx, fp);
    return Compile_Okay;
}

/*
 * Called from a backedge in the interpreter to decide if we should transition to the
 * methodjit. If so, we compile the given function.
 */
static inline CompileStatus
CanMethodJITAtBranch(JSContext *cx, JSScript *script, JSStackFrame *fp, jsbytecode *pc)
{
    if (!cx->methodJitEnabled)
        return Compile_Abort;
    JITScriptStatus status = script->getJITStatus(fp->isConstructing());
    if (status == JITScript_Invalid)
        return Compile_Abort;
    if (status == JITScript_None &&
        !cx->hasRunOption(JSOPTION_METHODJIT_ALWAYS) &&
        cx->compartment->incBackEdgeCount(pc) <= BACKEDGES_BEFORE_COMPILE)
    {
        return Compile_Skipped;
    }
    if (status == JITScript_None)
        return TryCompile(cx, fp);
    return Compile_Okay;
}

}
}

#endif
