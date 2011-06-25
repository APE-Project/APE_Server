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
 * JS script operations.
 */
#include <string.h>
#include "jstypes.h"
#include "jsstdint.h"
#include "jsutil.h"
#include "jsprf.h"
#include "jsapi.h"
#include "jsatom.h"
#include "jscntxt.h"
#include "jsversion.h"
#include "jsdbgapi.h"
#include "jsemit.h"
#include "jsfun.h"
#include "jsinterp.h"
#include "jslock.h"
#include "jsnum.h"
#include "jsopcode.h"
#include "jsparse.h"
#include "jsscope.h"
#include "jsscript.h"
#include "jstracer.h"
#if JS_HAS_XDR
#include "jsxdrapi.h"
#endif
#include "methodjit/MethodJIT.h"

#include "jsinterpinlines.h"
#include "jsobjinlines.h"
#include "jsscriptinlines.h"

using namespace js;
using namespace js::gc;

namespace js {

BindingKind
Bindings::lookup(JSContext *cx, JSAtom *name, uintN *indexp) const
{
    JS_ASSERT(lastBinding);

    Shape *shape =
        SHAPE_FETCH(Shape::search(cx->runtime, const_cast<Shape **>(&lastBinding),
                    ATOM_TO_JSID(name)));
    if (!shape)
        return NONE;

    if (indexp)
        *indexp = shape->shortid;

    if (shape->getter() == GetCallArg)
        return ARGUMENT;
    if (shape->getter() == GetCallUpvar)
        return UPVAR;

    return shape->writable() ? VARIABLE : CONSTANT;
}

bool
Bindings::add(JSContext *cx, JSAtom *name, BindingKind kind)
{
    JS_ASSERT(lastBinding);

    /*
     * We still follow 10.2.3 of ES3 and make argument and variable properties
     * of the Call objects enumerable. ES5 reformulated all of its Clause 10 to
     * avoid objects as activations, something we should do too.
     */
    uintN attrs = JSPROP_ENUMERATE | JSPROP_PERMANENT | JSPROP_SHARED;

    uint16 *indexp;
    PropertyOp getter;
    StrictPropertyOp setter;
    uint32 slot = JSObject::CALL_RESERVED_SLOTS;

    if (kind == ARGUMENT) {
        JS_ASSERT(nvars == 0);
        JS_ASSERT(nupvars == 0);
        indexp = &nargs;
        getter = GetCallArg;
        setter = SetCallArg;
        slot += nargs;
    } else if (kind == UPVAR) {
        indexp = &nupvars;
        getter = GetCallUpvar;
        setter = SetCallUpvar;
        slot = SHAPE_INVALID_SLOT;
    } else {
        JS_ASSERT(kind == VARIABLE || kind == CONSTANT);
        JS_ASSERT(nupvars == 0);

        indexp = &nvars;
        getter = GetCallVar;
        setter = SetCallVar;
        if (kind == CONSTANT)
            attrs |= JSPROP_READONLY;
        slot += nargs + nvars;
    }

    if (*indexp == BINDING_COUNT_LIMIT) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                             (kind == ARGUMENT)
                             ? JSMSG_TOO_MANY_FUN_ARGS
                             : JSMSG_TOO_MANY_LOCALS);
        return false;
    }

    jsid id;
    if (!name) {
        JS_ASSERT(kind == ARGUMENT); /* destructuring */
        id = INT_TO_JSID(nargs);
    } else {
        id = ATOM_TO_JSID(name);
    }

    Shape child(id, getter, setter, slot, attrs, Shape::HAS_SHORTID, *indexp);

    Shape *shape = lastBinding->getChild(cx, child, &lastBinding);
    if (!shape)
        return false;

    JS_ASSERT(lastBinding == shape);
    ++*indexp;
    return true;
}

jsuword *
Bindings::getLocalNameArray(JSContext *cx, JSArenaPool *pool)
{
   JS_ASSERT(lastBinding);

   JS_ASSERT(hasLocalNames());

    uintN n = countLocalNames();
    jsuword *names;

    JS_ASSERT(SIZE_MAX / size_t(n) > sizeof *names);
    JS_ARENA_ALLOCATE_CAST(names, jsuword *, pool, size_t(n) * sizeof *names);
    if (!names) {
        js_ReportOutOfScriptQuota(cx);
        return NULL;
    }

#ifdef DEBUG
    for (uintN i = 0; i != n; i++)
        names[i] = 0xdeadbeef;
#endif

    for (Shape::Range r = lastBinding; !r.empty(); r.popFront()) {
        const Shape &shape = r.front();
        uintN index = uint16(shape.shortid);
        jsuword constFlag = 0;

        if (shape.getter() == GetCallArg) {
            JS_ASSERT(index < nargs);
        } else if (shape.getter() == GetCallUpvar) {
            JS_ASSERT(index < nupvars);
            index += nargs + nvars;
        } else {
            JS_ASSERT(index < nvars);
            index += nargs;
            if (!shape.writable())
                constFlag = 1;
        }

        JSAtom *atom;
        if (JSID_IS_ATOM(shape.id)) {
            atom = JSID_TO_ATOM(shape.id);
        } else {
            JS_ASSERT(JSID_IS_INT(shape.id));
            JS_ASSERT(shape.getter() == GetCallArg);
            atom = NULL;
        }

        names[index] = jsuword(atom);
    }

#ifdef DEBUG
    for (uintN i = 0; i != n; i++)
        JS_ASSERT(names[i] != 0xdeadbeef);
#endif
    return names;
}

const Shape *
Bindings::lastArgument() const
{
    JS_ASSERT(lastBinding);

    const js::Shape *shape = lastVariable();
    if (nvars > 0) {
        while (shape->previous() && shape->getter() != GetCallArg)
            shape = shape->previous();
    }
    return shape;
}

const Shape *
Bindings::lastVariable() const
{
    JS_ASSERT(lastBinding);

    const js::Shape *shape = lastUpvar();
    if (nupvars > 0) {
        while (shape->getter() == GetCallUpvar)
            shape = shape->previous();
    }
    return shape;
}

const Shape *
Bindings::lastUpvar() const
{
    JS_ASSERT(lastBinding);
    return lastBinding;
}

int
Bindings::sharpSlotBase(JSContext *cx)
{
    JS_ASSERT(lastBinding);
#if JS_HAS_SHARP_VARS
    if (JSAtom *name = js_Atomize(cx, "#array", 6, 0)) {
        uintN index = uintN(-1);
#ifdef DEBUG
        BindingKind kind =
#endif
            lookup(cx, name, &index);
        JS_ASSERT(kind == VARIABLE);
        return int(index);
    }
#endif
    return -1;
}

void
Bindings::makeImmutable()
{
    JS_ASSERT(lastBinding);
    Shape *shape = lastBinding;
    if (shape->inDictionary()) {
        do {
            JS_ASSERT(!shape->frozen());
            shape->setFrozen();
        } while ((shape = shape->parent) != NULL);
    }
}

void
Bindings::trace(JSTracer *trc)
{
    for (const Shape *shape = lastBinding; shape; shape = shape->previous())
        shape->trace(trc);
}

} /* namespace js */

#if JS_HAS_XDR

enum ScriptBits {
    NoScriptRval,
    SavedCallerFun,
    HasSharps,
    StrictModeCode,
    UsesEval,
    UsesArguments
};

JSBool
js_XDRScript(JSXDRState *xdr, JSScript **scriptp, JSBool *hasMagic)
{
    JSScript *oldscript;
    JSBool ok;
    jsbytecode *code;
    uint32 length, lineno, nslots;
    uint32 natoms, nsrcnotes, ntrynotes, nobjects, nregexps, nconsts, i;
    uint32 prologLength, version, encodedClosedCount;
    uint16 nClosedArgs = 0, nClosedVars = 0;
    JSPrincipals *principals;
    uint32 encodeable;
    JSBool filenameWasSaved;
    jssrcnote *sn;
    JSSecurityCallbacks *callbacks;
    uint32 scriptBits = 0;

    JSContext *cx = xdr->cx;
    JSScript *script = *scriptp;
    nsrcnotes = ntrynotes = natoms = nobjects = nregexps = nconsts = 0;
    filenameWasSaved = JS_FALSE;
    jssrcnote *notes = NULL;

    /* Should not XDR scripts optimized for a single global object. */
    JS_ASSERT_IF(script, !JSScript::isValidOffset(script->globalsOffset));

    uint32 magic;
    if (xdr->mode == JSXDR_ENCODE)
        magic = JSXDR_MAGIC_SCRIPT_CURRENT;
    if (!JS_XDRUint32(xdr, &magic))
        return JS_FALSE;
    if (magic != JSXDR_MAGIC_SCRIPT_CURRENT) {
        /* We do not provide binary compatibility with older scripts. */
        if (!hasMagic) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_BAD_SCRIPT_MAGIC);
            return JS_FALSE;
        }
        *hasMagic = JS_FALSE;
        return JS_TRUE;
    }
    if (hasMagic)
        *hasMagic = JS_TRUE;

    /* XDR arguments, local vars, and upvars. */
    uint16 nargs, nvars, nupvars;
#if defined(DEBUG) || defined(__GNUC__) /* quell GCC overwarning */
    nargs = nvars = nupvars = Bindings::BINDING_COUNT_LIMIT;
#endif
    uint32 argsVars, paddingUpvars;
    if (xdr->mode == JSXDR_ENCODE) {
        nargs = script->bindings.countArgs();
        nvars = script->bindings.countVars();
        nupvars = script->bindings.countUpvars();
        argsVars = (nargs << 16) | nvars;
        paddingUpvars = nupvars;
    }
    if (!JS_XDRUint32(xdr, &argsVars) || !JS_XDRUint32(xdr, &paddingUpvars))
        return false;
    if (xdr->mode == JSXDR_DECODE) {
        nargs = argsVars >> 16;
        nvars = argsVars & 0xFFFF;
        JS_ASSERT((paddingUpvars >> 16) == 0);
        nupvars = paddingUpvars & 0xFFFF;
    }
    JS_ASSERT(nargs != Bindings::BINDING_COUNT_LIMIT);
    JS_ASSERT(nvars != Bindings::BINDING_COUNT_LIMIT);
    JS_ASSERT(nupvars != Bindings::BINDING_COUNT_LIMIT);

    Bindings bindings(cx);
    AutoBindingsRooter rooter(cx, bindings);
    uint32 nameCount = nargs + nvars + nupvars;
    if (nameCount > 0) {
        struct AutoMark {
          JSArenaPool * const pool;
          void * const mark;
          AutoMark(JSArenaPool *pool) : pool(pool), mark(JS_ARENA_MARK(pool)) { }
          ~AutoMark() {
            JS_ARENA_RELEASE(pool, mark);
          }
        } automark(&cx->tempPool);

        /*
         * To xdr the names we prefix the names with a bitmap descriptor and
         * then xdr the names as strings. For argument names (indexes below
         * nargs) the corresponding bit in the bitmap is unset when the name
         * is null. Such null names are not encoded or decoded. For variable
         * names (indexes starting from nargs) bitmap's bit is set when the
         * name is declared as const, not as ordinary var.
         * */
        uintN bitmapLength = JS_HOWMANY(nameCount, JS_BITS_PER_UINT32);
        uint32 *bitmap;
        JS_ARENA_ALLOCATE_CAST(bitmap, uint32 *, &cx->tempPool,
                               bitmapLength * sizeof *bitmap);
        if (!bitmap) {
            js_ReportOutOfScriptQuota(cx);
            return false;
        }

        jsuword *names;
        if (xdr->mode == JSXDR_ENCODE) {
            names = script->bindings.getLocalNameArray(cx, &cx->tempPool);
            if (!names)
                return false;
            PodZero(bitmap, bitmapLength);
            for (uintN i = 0; i < nameCount; i++) {
                if (i < nargs
                    ? JS_LOCAL_NAME_TO_ATOM(names[i]) != NULL
                    : JS_LOCAL_NAME_IS_CONST(names[i]))
                {
                    bitmap[i >> JS_BITS_PER_UINT32_LOG2] |= JS_BIT(i & (JS_BITS_PER_UINT32 - 1));
                }
            }
        }
#ifdef __GNUC__
        else {
            names = NULL;   /* quell GCC uninitialized warning */
        }
#endif
        for (uintN i = 0; i < bitmapLength; ++i) {
            if (!JS_XDRUint32(xdr, &bitmap[i]))
                return false;
        }

        for (uintN i = 0; i < nameCount; i++) {
            if (i < nargs &&
                !(bitmap[i >> JS_BITS_PER_UINT32_LOG2] & JS_BIT(i & (JS_BITS_PER_UINT32 - 1))))
            {
                if (xdr->mode == JSXDR_DECODE) {
                    uint16 dummy;
                    if (!bindings.addDestructuring(cx, &dummy))
                        return false;
                } else {
                    JS_ASSERT(!JS_LOCAL_NAME_TO_ATOM(names[i]));
                }
                continue;
            }

            JSAtom *name;
            if (xdr->mode == JSXDR_ENCODE)
                name = JS_LOCAL_NAME_TO_ATOM(names[i]);
            if (!js_XDRAtom(xdr, &name))
                return false;
            if (xdr->mode == JSXDR_DECODE) {
                BindingKind kind = (i < nargs)
                                   ? ARGUMENT
                                   : (i < uintN(nargs + nvars))
                                   ? (bitmap[i >> JS_BITS_PER_UINT32_LOG2] &
                                      JS_BIT(i & (JS_BITS_PER_UINT32 - 1))
                                      ? CONSTANT
                                      : VARIABLE)
                                   : UPVAR;
                if (!bindings.add(cx, name, kind))
                    return false;
            }
        }

        if (xdr->mode == JSXDR_DECODE)
            bindings.makeImmutable();
    }

    if (xdr->mode == JSXDR_ENCODE)
        length = script->length;
    if (!JS_XDRUint32(xdr, &length))
        return JS_FALSE;

    if (xdr->mode == JSXDR_ENCODE) {
        prologLength = script->main - script->code;
        JS_ASSERT(script->getVersion() != JSVERSION_UNKNOWN);
        version = (uint32)script->getVersion() | (script->nfixed << 16);
        lineno = (uint32)script->lineno;
        nslots = (uint32)script->nslots;
        nslots = (uint32)((script->staticLevel << 16) | script->nslots);
        natoms = (uint32)script->atomMap.length;

        /* Count the srcnotes, keeping notes pointing at the first one. */
        notes = script->notes();
        for (sn = notes; !SN_IS_TERMINATOR(sn); sn = SN_NEXT(sn))
            continue;
        nsrcnotes = sn - notes;
        nsrcnotes++;            /* room for the terminator */

        if (JSScript::isValidOffset(script->objectsOffset))
            nobjects = script->objects()->length;
        if (JSScript::isValidOffset(script->upvarsOffset))
            JS_ASSERT(script->bindings.countUpvars() == script->upvars()->length);
        if (JSScript::isValidOffset(script->regexpsOffset))
            nregexps = script->regexps()->length;
        if (JSScript::isValidOffset(script->trynotesOffset))
            ntrynotes = script->trynotes()->length;
        if (JSScript::isValidOffset(script->constOffset))
            nconsts = script->consts()->length;

        nClosedArgs = script->nClosedArgs;
        nClosedVars = script->nClosedVars;
        encodedClosedCount = (nClosedArgs << 16) | nClosedVars;

        if (script->noScriptRval)
            scriptBits |= (1 << NoScriptRval);
        if (script->savedCallerFun)
            scriptBits |= (1 << SavedCallerFun);
        if (script->hasSharps)
            scriptBits |= (1 << HasSharps);
        if (script->strictModeCode)
            scriptBits |= (1 << StrictModeCode);
        if (script->usesEval)
            scriptBits |= (1 << UsesEval);
        if (script->usesArguments)
            scriptBits |= (1 << UsesArguments);
        JS_ASSERT(!script->compileAndGo);
        JS_ASSERT(!script->hasSingletons);
    }

    if (!JS_XDRUint32(xdr, &prologLength))
        return JS_FALSE;
    if (!JS_XDRUint32(xdr, &version))
        return JS_FALSE;

    /*
     * To fuse allocations, we need srcnote, atom, objects, regexp, and trynote
     * counts early.
     */
    if (!JS_XDRUint32(xdr, &natoms))
        return JS_FALSE;
    if (!JS_XDRUint32(xdr, &nsrcnotes))
        return JS_FALSE;
    if (!JS_XDRUint32(xdr, &ntrynotes))
        return JS_FALSE;
    if (!JS_XDRUint32(xdr, &nobjects))
        return JS_FALSE;
    if (!JS_XDRUint32(xdr, &nregexps))
        return JS_FALSE;
    if (!JS_XDRUint32(xdr, &nconsts))
        return JS_FALSE;
    if (!JS_XDRUint32(xdr, &encodedClosedCount))
        return JS_FALSE;
    if (!JS_XDRUint32(xdr, &scriptBits))
        return JS_FALSE;

    AutoScriptRooter tvr(cx, NULL);

    if (xdr->mode == JSXDR_DECODE) {
        nClosedArgs = encodedClosedCount >> 16;
        nClosedVars = encodedClosedCount & 0xFFFF;

        /* Note: version is packed into the 32b space with another 16b value. */
        JSVersion version_ = JSVersion(version & JS_BITMASK(16));
        JS_ASSERT((version_ & VersionFlags::FULL_MASK) == uintN(version_));
        script = JSScript::NewScript(cx, length, nsrcnotes, natoms, nobjects, nupvars,
                                     nregexps, ntrynotes, nconsts, 0, nClosedArgs,
                                     nClosedVars, version_);
        if (!script)
            return JS_FALSE;

        script->bindings.transfer(cx, &bindings);

        script->main += prologLength;
        script->nfixed = uint16(version >> 16);

        /* If we know nsrcnotes, we allocated space for notes in script. */
        notes = script->notes();
        *scriptp = script;
        tvr.setScript(script);

        if (scriptBits & (1 << NoScriptRval))
            script->noScriptRval = true;
        if (scriptBits & (1 << SavedCallerFun))
            script->savedCallerFun = true;
        if (scriptBits & (1 << HasSharps))
            script->hasSharps = true;
        if (scriptBits & (1 << StrictModeCode))
            script->strictModeCode = true;
        if (scriptBits & (1 << UsesEval))
            script->usesEval = true;
        if (scriptBits & (1 << UsesArguments))
            script->usesArguments = true;
    }

    /*
     * Control hereafter must goto error on failure, in order for the
     * DECODE case to destroy script.
     */
    oldscript = xdr->script;
    code = script->code;
    if (xdr->mode == JSXDR_ENCODE) {
        code = js_UntrapScriptCode(cx, script);
        if (!code)
            goto error;
    }

    xdr->script = script;
    ok = JS_XDRBytes(xdr, (char *) code, length * sizeof(jsbytecode));

    if (code != script->code)
        cx->free(code);

    if (!ok)
        goto error;

    if (!JS_XDRBytes(xdr, (char *)notes, nsrcnotes * sizeof(jssrcnote)) ||
        !JS_XDRCStringOrNull(xdr, (char **)&script->filename) ||
        !JS_XDRUint32(xdr, &lineno) ||
        !JS_XDRUint32(xdr, &nslots)) {
        goto error;
    }

    callbacks = JS_GetSecurityCallbacks(cx);
    if (xdr->mode == JSXDR_ENCODE) {
        principals = script->principals;
        encodeable = callbacks && callbacks->principalsTranscoder;
        if (!JS_XDRUint32(xdr, &encodeable))
            goto error;
        if (encodeable &&
            !callbacks->principalsTranscoder(xdr, &principals)) {
            goto error;
        }
    } else {
        if (!JS_XDRUint32(xdr, &encodeable))
            goto error;
        if (encodeable) {
            if (!(callbacks && callbacks->principalsTranscoder)) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                     JSMSG_CANT_DECODE_PRINCIPALS);
                goto error;
            }
            if (!callbacks->principalsTranscoder(xdr, &principals))
                goto error;
            script->principals = principals;
        }
    }

    if (xdr->mode == JSXDR_DECODE) {
        const char *filename = script->filename;
        if (filename) {
            filename = js_SaveScriptFilename(cx, filename);
            if (!filename)
                goto error;
            cx->free((void *) script->filename);
            script->filename = filename;
            filenameWasSaved = JS_TRUE;
        }
        script->lineno = (uintN)lineno;
        script->nslots = (uint16)nslots;
        script->staticLevel = (uint16)(nslots >> 16);

    }

    for (i = 0; i != natoms; ++i) {
        if (!js_XDRAtom(xdr, &script->atomMap.vector[i]))
            goto error;
    }

    /*
     * Here looping from 0-to-length to xdr objects is essential. It ensures
     * that block objects from the script->objects array will be written and
     * restored in the outer-to-inner order. js_XDRBlockObject relies on this
     * to restore the parent chain.
     */
    for (i = 0; i != nobjects; ++i) {
        JSObject **objp = &script->objects()->vector[i];
        uint32 isBlock;
        if (xdr->mode == JSXDR_ENCODE) {
            Class *clasp = (*objp)->getClass();
            JS_ASSERT(clasp == &js_FunctionClass ||
                      clasp == &js_BlockClass);
            isBlock = (clasp == &js_BlockClass) ? 1 : 0;
        }
        if (!JS_XDRUint32(xdr, &isBlock))
            goto error;
        if (isBlock == 0) {
            if (!js_XDRFunctionObject(xdr, objp))
                goto error;
        } else {
            JS_ASSERT(isBlock == 1);
            if (!js_XDRBlockObject(xdr, objp))
                goto error;
        }
    }
    for (i = 0; i != nupvars; ++i) {
        if (!JS_XDRUint32(xdr, reinterpret_cast<uint32 *>(&script->upvars()->vector[i])))
            goto error;
    }
    for (i = 0; i != nregexps; ++i) {
        if (!js_XDRRegExpObject(xdr, &script->regexps()->vector[i]))
            goto error;
    }
    for (i = 0; i != nClosedArgs; ++i) {
        if (!JS_XDRUint32(xdr, &script->closedSlots[i]))
            goto error;
    }
    for (i = 0; i != nClosedVars; ++i) {
        if (!JS_XDRUint32(xdr, &script->closedSlots[nClosedArgs + i]))
            goto error;
    }

    if (ntrynotes != 0) {
        /*
         * We combine tn->kind and tn->stackDepth when serializing as XDR is not
         * efficient when serializing small integer types.
         */
        JSTryNote *tn, *tnfirst;
        uint32 kindAndDepth;
        JS_STATIC_ASSERT(sizeof(tn->kind) == sizeof(uint8));
        JS_STATIC_ASSERT(sizeof(tn->stackDepth) == sizeof(uint16));

        tnfirst = script->trynotes()->vector;
        JS_ASSERT(script->trynotes()->length == ntrynotes);
        tn = tnfirst + ntrynotes;
        do {
            --tn;
            if (xdr->mode == JSXDR_ENCODE) {
                kindAndDepth = ((uint32)tn->kind << 16)
                               | (uint32)tn->stackDepth;
            }
            if (!JS_XDRUint32(xdr, &kindAndDepth) ||
                !JS_XDRUint32(xdr, &tn->start) ||
                !JS_XDRUint32(xdr, &tn->length)) {
                goto error;
            }
            if (xdr->mode == JSXDR_DECODE) {
                tn->kind = (uint8)(kindAndDepth >> 16);
                tn->stackDepth = (uint16)kindAndDepth;
            }
        } while (tn != tnfirst);
    }

    for (i = 0; i != nconsts; ++i) {
        if (!JS_XDRValue(xdr, Jsvalify(&script->consts()->vector[i])))
            goto error;
    }

    xdr->script = oldscript;
    return JS_TRUE;

  error:
    if (xdr->mode == JSXDR_DECODE) {
        if (script->filename && !filenameWasSaved) {
            cx->free((void *) script->filename);
            script->filename = NULL;
        }
        js_DestroyScript(cx, script);
        *scriptp = NULL;
    }
    xdr->script = oldscript;
    return JS_FALSE;
}

#endif /* JS_HAS_XDR */

static void
script_finalize(JSContext *cx, JSObject *obj)
{
    JSScript *script = (JSScript *) obj->getPrivate();
    if (script)
        js_DestroyScriptFromGC(cx, script);
}

static void
script_trace(JSTracer *trc, JSObject *obj)
{
    JSScript *script = (JSScript *) obj->getPrivate();
    if (script)
        js_TraceScript(trc, script);
}

Class js_ScriptClass = {
    "Script",
    JSCLASS_HAS_PRIVATE |
    JSCLASS_MARK_IS_TRACE | JSCLASS_HAS_CACHED_PROTO(JSProto_Object),
    PropertyStub,         /* addProperty */
    PropertyStub,         /* delProperty */
    PropertyStub,         /* getProperty */
    StrictPropertyStub,   /* setProperty */
    EnumerateStub,
    ResolveStub,
    ConvertStub,
    script_finalize,
    NULL,                 /* reserved0   */
    NULL,                 /* checkAccess */
    NULL,                 /* call        */
    NULL,                 /* construct   */
    NULL,                 /* xdrObject   */
    NULL,                 /* hasInstance */
    JS_CLASS_TRACE(script_trace)
};

/*
 * Shared script filename management.
 */
static int
js_compare_strings(const void *k1, const void *k2)
{
    return strcmp((const char *) k1, (const char *) k2) == 0;
}

/* NB: This struct overlays JSHashEntry -- see jshash.h, do not reorganize. */
typedef struct ScriptFilenameEntry {
    JSHashEntry         *next;          /* hash chain linkage */
    JSHashNumber        keyHash;        /* key hash function result */
    const void          *key;           /* ptr to filename, below */
    uint32              flags;          /* user-defined filename prefix flags */
    JSPackedBool        mark;           /* GC mark flag */
    char                filename[3];    /* two or more bytes, NUL-terminated */
} ScriptFilenameEntry;

static void *
js_alloc_table_space(void *priv, size_t size)
{
    return js_malloc(size);
}

static void
js_free_table_space(void *priv, void *item, size_t size)
{
    js_free(item);
}

static JSHashEntry *
js_alloc_sftbl_entry(void *priv, const void *key)
{
    size_t nbytes = offsetof(ScriptFilenameEntry, filename) +
                    strlen((const char *) key) + 1;

    return (JSHashEntry *) js_malloc(JS_MAX(nbytes, sizeof(JSHashEntry)));
}

static void
js_free_sftbl_entry(void *priv, JSHashEntry *he, uintN flag)
{
    if (flag != HT_FREE_ENTRY)
        return;
    js_free(he);
}

static JSHashAllocOps sftbl_alloc_ops = {
    js_alloc_table_space,   js_free_table_space,
    js_alloc_sftbl_entry,   js_free_sftbl_entry
};

static void
FinishRuntimeScriptState(JSRuntime *rt)
{
    if (rt->scriptFilenameTable) {
        JS_HashTableDestroy(rt->scriptFilenameTable);
        rt->scriptFilenameTable = NULL;
    }
#ifdef JS_THREADSAFE
    if (rt->scriptFilenameTableLock) {
        JS_DESTROY_LOCK(rt->scriptFilenameTableLock);
        rt->scriptFilenameTableLock = NULL;
    }
#endif
}

JSBool
js_InitRuntimeScriptState(JSRuntime *rt)
{
#ifdef JS_THREADSAFE
    JS_ASSERT(!rt->scriptFilenameTableLock);
    rt->scriptFilenameTableLock = JS_NEW_LOCK();
    if (!rt->scriptFilenameTableLock)
        return JS_FALSE;
#endif
    JS_ASSERT(!rt->scriptFilenameTable);
    rt->scriptFilenameTable =
        JS_NewHashTable(16, JS_HashString, js_compare_strings, NULL,
                        &sftbl_alloc_ops, NULL);
    if (!rt->scriptFilenameTable) {
        FinishRuntimeScriptState(rt);       /* free lock if threadsafe */
        return JS_FALSE;
    }
    JS_INIT_CLIST(&rt->scriptFilenamePrefixes);
    return JS_TRUE;
}

typedef struct ScriptFilenamePrefix {
    JSCList     links;      /* circular list linkage for easy deletion */
    const char  *name;      /* pointer to pinned ScriptFilenameEntry string */
    size_t      length;     /* prefix string length, precomputed */
    uint32      flags;      /* user-defined flags to inherit from this prefix */
} ScriptFilenamePrefix;

void
js_FreeRuntimeScriptState(JSRuntime *rt)
{
    if (!rt->scriptFilenameTable)
        return;

    while (!JS_CLIST_IS_EMPTY(&rt->scriptFilenamePrefixes)) {
        ScriptFilenamePrefix *sfp = (ScriptFilenamePrefix *)
                                    rt->scriptFilenamePrefixes.next;
        JS_REMOVE_LINK(&sfp->links);
        js_free(sfp);
    }
    FinishRuntimeScriptState(rt);
}

#ifdef DEBUG_brendan
#define DEBUG_SFTBL
#endif
#ifdef DEBUG_SFTBL
size_t sftbl_savings = 0;
#endif

static ScriptFilenameEntry *
SaveScriptFilename(JSRuntime *rt, const char *filename, uint32 flags)
{
    JSHashTable *table;
    JSHashNumber hash;
    JSHashEntry **hep;
    ScriptFilenameEntry *sfe;
    size_t length;
    JSCList *head, *link;
    ScriptFilenamePrefix *sfp;

    table = rt->scriptFilenameTable;
    hash = JS_HashString(filename);
    hep = JS_HashTableRawLookup(table, hash, filename);
    sfe = (ScriptFilenameEntry *) *hep;
#ifdef DEBUG_SFTBL
    if (sfe)
        sftbl_savings += strlen(sfe->filename);
#endif

    if (!sfe) {
        sfe = (ScriptFilenameEntry *)
              JS_HashTableRawAdd(table, hep, hash, filename, NULL);
        if (!sfe)
            return NULL;
        sfe->key = strcpy(sfe->filename, filename);
        sfe->flags = 0;
        sfe->mark = JS_FALSE;
    }

    /* If saving a prefix, add it to the set in rt->scriptFilenamePrefixes. */
    if (flags != 0) {
        /* Search in case filename was saved already; we must be idempotent. */
        sfp = NULL;
        length = strlen(filename);
        for (head = link = &rt->scriptFilenamePrefixes;
             link->next != head;
             link = link->next) {
            /* Lag link behind sfp to insert in non-increasing length order. */
            sfp = (ScriptFilenamePrefix *) link->next;
            if (!strcmp(sfp->name, filename))
                break;
            if (sfp->length <= length) {
                sfp = NULL;
                break;
            }
            sfp = NULL;
        }

        if (!sfp) {
            /* No such prefix: add one now. */
            sfp = (ScriptFilenamePrefix *) js_malloc(sizeof(ScriptFilenamePrefix));
            if (!sfp)
                return NULL;
            JS_INSERT_AFTER(&sfp->links, link);
            sfp->name = sfe->filename;
            sfp->length = length;
            sfp->flags = 0;
        }

        /*
         * Accumulate flags in both sfe and sfp: sfe for later access from the
         * JS_GetScriptedCallerFilenameFlags debug-API, and sfp so that longer
         * filename entries can inherit by prefix.
         */
        sfe->flags |= flags;
        sfp->flags |= flags;
    }

#ifdef DEBUG
    if (rt->functionMeterFilename) {
        size_t len = strlen(sfe->filename);
        if (len >= sizeof rt->lastScriptFilename)
            len = sizeof rt->lastScriptFilename - 1;
        memcpy(rt->lastScriptFilename, sfe->filename, len);
        rt->lastScriptFilename[len] = '\0';
    }
#endif

    return sfe;
}

const char *
js_SaveScriptFilename(JSContext *cx, const char *filename)
{
    JSRuntime *rt;
    ScriptFilenameEntry *sfe;
    JSCList *head, *link;
    ScriptFilenamePrefix *sfp;

    rt = cx->runtime;
    JS_ACQUIRE_LOCK(rt->scriptFilenameTableLock);
    sfe = SaveScriptFilename(rt, filename, 0);
    if (!sfe) {
        JS_RELEASE_LOCK(rt->scriptFilenameTableLock);
        JS_ReportOutOfMemory(cx);
        return NULL;
    }

    /*
     * Try to inherit flags by prefix.  We assume there won't be more than a
     * few (dozen! ;-) prefixes, so linear search is tolerable.
     * XXXbe every time I've assumed that in the JS engine, I've been wrong!
     */
    for (head = &rt->scriptFilenamePrefixes, link = head->next;
         link != head;
         link = link->next) {
        sfp = (ScriptFilenamePrefix *) link;
        if (!strncmp(sfp->name, filename, sfp->length)) {
            sfe->flags |= sfp->flags;
            break;
        }
    }
    JS_RELEASE_LOCK(rt->scriptFilenameTableLock);
    return sfe->filename;
}

const char *
js_SaveScriptFilenameRT(JSRuntime *rt, const char *filename, uint32 flags)
{
    ScriptFilenameEntry *sfe;

    /* This may be called very early, via the jsdbgapi.h entry point. */
    if (!rt->scriptFilenameTable && !js_InitRuntimeScriptState(rt))
        return NULL;

    JS_ACQUIRE_LOCK(rt->scriptFilenameTableLock);
    sfe = SaveScriptFilename(rt, filename, flags);
    JS_RELEASE_LOCK(rt->scriptFilenameTableLock);
    if (!sfe)
        return NULL;

    return sfe->filename;
}

/*
 * Back up from a saved filename by its offset within its hash table entry.
 */
#define FILENAME_TO_SFE(fn) \
    ((ScriptFilenameEntry *) ((fn) - offsetof(ScriptFilenameEntry, filename)))

/*
 * The sfe->key member, redundant given sfe->filename but required by the old
 * jshash.c code, here gives us a useful sanity check.  This assertion will
 * very likely botch if someone tries to mark a string that wasn't allocated
 * as an sfe->filename.
 */
#define ASSERT_VALID_SFE(sfe)   JS_ASSERT((sfe)->key == (sfe)->filename)

uint32
js_GetScriptFilenameFlags(const char *filename)
{
    ScriptFilenameEntry *sfe;

    sfe = FILENAME_TO_SFE(filename);
    ASSERT_VALID_SFE(sfe);
    return sfe->flags;
}

void
js_MarkScriptFilename(const char *filename)
{
    ScriptFilenameEntry *sfe;

    sfe = FILENAME_TO_SFE(filename);
    ASSERT_VALID_SFE(sfe);
    sfe->mark = JS_TRUE;
}

static intN
js_script_filename_marker(JSHashEntry *he, intN i, void *arg)
{
    ScriptFilenameEntry *sfe = (ScriptFilenameEntry *) he;

    sfe->mark = JS_TRUE;
    return HT_ENUMERATE_NEXT;
}

void
js_MarkScriptFilenames(JSRuntime *rt)
{
    JSCList *head, *link;
    ScriptFilenamePrefix *sfp;

    if (!rt->scriptFilenameTable)
        return;

    if (rt->gcKeepAtoms) {
        JS_HashTableEnumerateEntries(rt->scriptFilenameTable,
                                     js_script_filename_marker,
                                     rt);
    }
    for (head = &rt->scriptFilenamePrefixes, link = head->next;
         link != head;
         link = link->next) {
        sfp = (ScriptFilenamePrefix *) link;
        js_MarkScriptFilename(sfp->name);
    }
}

static intN
js_script_filename_sweeper(JSHashEntry *he, intN i, void *arg)
{
    ScriptFilenameEntry *sfe = (ScriptFilenameEntry *) he;

    if (!sfe->mark)
        return HT_ENUMERATE_REMOVE;
    sfe->mark = JS_FALSE;
    return HT_ENUMERATE_NEXT;
}

void
js_SweepScriptFilenames(JSRuntime *rt)
{
    if (!rt->scriptFilenameTable)
        return;

    /*
     * JS_HashTableEnumerateEntries shrinks the table if many entries are
     * removed preventing wasting memory on a too sparse table.
     */
    JS_HashTableEnumerateEntries(rt->scriptFilenameTable,
                                 js_script_filename_sweeper,
                                 rt);
#ifdef DEBUG_notme
#ifdef DEBUG_SFTBL
    printf("script filename table savings so far: %u\n", sftbl_savings);
#endif
#endif
}

/*
 * JSScript data structures memory alignment:
 *
 * JSScript
 * JSObjectArray    script objects' descriptor if JSScript.objectsOffset != 0,
 *                    use script->objects() to access it.
 * JSObjectArray    script regexps' descriptor if JSScript.regexpsOffset != 0,
 *                    use script->regexps() to access it.
 * JSTryNoteArray   script try notes' descriptor if JSScript.tryNotesOffset
 *                    != 0, use script->trynotes() to access it.
 * JSAtom *a[]      array of JSScript.atomMap.length atoms pointed by
 *                    JSScript.atomMap.vector if any.
 * JSObject *o[]    array of script->objects()->length objects if any
 *                    pointed by script->objects()->vector.
 * JSObject *r[]    array of script->regexps()->length regexps if any
 *                    pointed by script->regexps()->vector.
 * JSTryNote t[]    array of script->trynotes()->length try notes if any
 *                    pointed by script->trynotes()->vector.
 * jsbytecode b[]   script bytecode pointed by JSScript.code.
 * jssrcnote  s[]   script source notes, use script->notes() to access it
 *
 * The alignment avoids gaps between entries as alignment requirement for each
 * subsequent structure or array is the same or divides the alignment
 * requirement for the previous one.
 *
 * The followings asserts checks that assuming that the alignment requirement
 * for JSObjectArray and JSTryNoteArray are sizeof(void *) and for JSTryNote
 * it is sizeof(uint32) as the structure consists of 3 uint32 fields.
 */
JS_STATIC_ASSERT(sizeof(JSScript) % sizeof(void *) == 0);
JS_STATIC_ASSERT(sizeof(JSObjectArray) % sizeof(void *) == 0);
JS_STATIC_ASSERT(sizeof(JSTryNoteArray) == sizeof(JSObjectArray));
JS_STATIC_ASSERT(sizeof(JSAtom *) == sizeof(JSObject *));
JS_STATIC_ASSERT(sizeof(JSObject *) % sizeof(uint32) == 0);
JS_STATIC_ASSERT(sizeof(JSTryNote) == 3 * sizeof(uint32));
JS_STATIC_ASSERT(sizeof(uint32) % sizeof(jsbytecode) == 0);
JS_STATIC_ASSERT(sizeof(jsbytecode) % sizeof(jssrcnote) == 0);

/*
 * Check that uint8 offsets is enough to reach any optional array allocated
 * after JSScript. For that we check that the maximum possible offset for
 * JSConstArray, that last optional array, still fits 1 byte and do not
 * coincide with INVALID_OFFSET.
 */
JS_STATIC_ASSERT(sizeof(JSObjectArray) +
                 sizeof(JSUpvarArray) +
                 sizeof(JSObjectArray) +
                 sizeof(JSTryNoteArray) +
                 sizeof(js::GlobalSlotArray)
                 < JSScript::INVALID_OFFSET);
JS_STATIC_ASSERT(JSScript::INVALID_OFFSET <= 255);

JSScript *
JSScript::NewScript(JSContext *cx, uint32 length, uint32 nsrcnotes, uint32 natoms,
                    uint32 nobjects, uint32 nupvars, uint32 nregexps,
                    uint32 ntrynotes, uint32 nconsts, uint32 nglobals,
                    uint16 nClosedArgs, uint16 nClosedVars, JSVersion version)
{
    size_t size, vectorSize;
    JSScript *script;
    uint8 *cursor;
    unsigned constPadding = 0;

    uint32 totalClosed = nClosedArgs + nClosedVars;

    size = sizeof(JSScript) +
           sizeof(JSAtom *) * natoms;
    
    if (nobjects != 0)
        size += sizeof(JSObjectArray) + nobjects * sizeof(JSObject *);
    if (nupvars != 0)
        size += sizeof(JSUpvarArray) + nupvars * sizeof(uint32);
    if (nregexps != 0)
        size += sizeof(JSObjectArray) + nregexps * sizeof(JSObject *);
    if (ntrynotes != 0)
        size += sizeof(JSTryNoteArray) + ntrynotes * sizeof(JSTryNote);
    if (nglobals != 0)
        size += sizeof(GlobalSlotArray) + nglobals * sizeof(GlobalSlotArray::Entry);
    if (totalClosed != 0)
        size += totalClosed * sizeof(uint32);

    if (nconsts != 0) {
        size += sizeof(JSConstArray);
        /*
         * Calculate padding assuming that consts go after the other arrays,
         * but before the bytecode and source notes.
         */
        constPadding = (8 - (size % 8)) % 8;
        size += constPadding + nconsts * sizeof(Value);
    }

    size += length * sizeof(jsbytecode) +
            nsrcnotes * sizeof(jssrcnote);

    script = (JSScript *) cx->malloc(size);
    if (!script)
        return NULL;

    PodZero(script);
    script->length = length;
    script->version = version;
    new (&script->bindings) Bindings(cx);

    uint8 *scriptEnd = reinterpret_cast<uint8 *>(script + 1);

    cursor = scriptEnd;
    if (nobjects != 0) {
        script->objectsOffset = (uint8)(cursor - scriptEnd);
        cursor += sizeof(JSObjectArray);
    } else {
        script->objectsOffset = JSScript::INVALID_OFFSET;
    }
    if (nupvars != 0) {
        script->upvarsOffset = (uint8)(cursor - scriptEnd);
        cursor += sizeof(JSUpvarArray);
    } else {
        script->upvarsOffset = JSScript::INVALID_OFFSET;
    }
    if (nregexps != 0) {
        script->regexpsOffset = (uint8)(cursor - scriptEnd);
        cursor += sizeof(JSObjectArray);
    } else {
        script->regexpsOffset = JSScript::INVALID_OFFSET;
    }
    if (ntrynotes != 0) {
        script->trynotesOffset = (uint8)(cursor - scriptEnd);
        cursor += sizeof(JSTryNoteArray);
    } else {
        script->trynotesOffset = JSScript::INVALID_OFFSET;
    }
    if (nglobals != 0) {
        script->globalsOffset = (uint8)(cursor - scriptEnd);
        cursor += sizeof(GlobalSlotArray);
    } else {
        script->globalsOffset = JSScript::INVALID_OFFSET;
    }
    JS_ASSERT(cursor - scriptEnd < 0xFF);
    if (nconsts != 0) {
        script->constOffset = (uint8)(cursor - scriptEnd);
        cursor += sizeof(JSConstArray);
    } else {
        script->constOffset = JSScript::INVALID_OFFSET;
    }

    JS_STATIC_ASSERT(sizeof(JSObjectArray) +
                     sizeof(JSUpvarArray) +
                     sizeof(JSObjectArray) +
                     sizeof(JSTryNoteArray) +
                     sizeof(GlobalSlotArray) < 0xFF);

    if (natoms != 0) {
        script->atomMap.length = natoms;
        script->atomMap.vector = (JSAtom **)cursor;
        vectorSize = natoms * sizeof(script->atomMap.vector[0]);

        /*
         * Clear object map's vector so the GC tracing can run when not yet
         * all atoms are copied to the array.
         */
        memset(cursor, 0, vectorSize);
        cursor += vectorSize;
    }

    if (nobjects != 0) {
        script->objects()->length = nobjects;
        script->objects()->vector = (JSObject **)cursor;
        vectorSize = nobjects * sizeof(script->objects()->vector[0]);
        memset(cursor, 0, vectorSize);
        cursor += vectorSize;
    }

    if (nregexps != 0) {
        script->regexps()->length = nregexps;
        script->regexps()->vector = (JSObject **)cursor;
        vectorSize = nregexps * sizeof(script->regexps()->vector[0]);
        memset(cursor, 0, vectorSize);
        cursor += vectorSize;
    }

    if (ntrynotes != 0) {
        script->trynotes()->length = ntrynotes;
        script->trynotes()->vector = (JSTryNote *)cursor;
        vectorSize = ntrynotes * sizeof(script->trynotes()->vector[0]);
#ifdef DEBUG
        memset(cursor, 0, vectorSize);
#endif
        cursor += vectorSize;
    }

    if (nglobals != 0) {
        script->globals()->length = nglobals;
        script->globals()->vector = (GlobalSlotArray::Entry *)cursor;
        vectorSize = nglobals * sizeof(script->globals()->vector[0]);
        cursor += vectorSize;
    }

    if (totalClosed != 0) {
        script->nClosedArgs = nClosedArgs;
        script->nClosedVars = nClosedVars;
        script->closedSlots = (uint32 *)cursor;
        cursor += totalClosed * sizeof(uint32);
    }

    /*
     * NB: We allocate the vector of uint32 upvar cookies after all vectors of
     * pointers, to avoid misalignment on 64-bit platforms. See bug 514645.
     */
    if (nupvars != 0) {
        script->upvars()->length = nupvars;
        script->upvars()->vector = reinterpret_cast<UpvarCookie *>(cursor);
        vectorSize = nupvars * sizeof(script->upvars()->vector[0]);
        memset(cursor, 0, vectorSize);
        cursor += vectorSize;
    }

    /* Must go after other arrays; see constPadding definition. */
    if (nconsts != 0) {
        cursor += constPadding;
        script->consts()->length = nconsts;
        script->consts()->vector = (Value *)cursor;
        JS_ASSERT((size_t)cursor % sizeof(double) == 0);
        vectorSize = nconsts * sizeof(script->consts()->vector[0]);
        memset(cursor, 0, vectorSize);
        cursor += vectorSize;
    }

    script->code = script->main = (jsbytecode *)cursor;
    JS_ASSERT(cursor +
              length * sizeof(jsbytecode) +
              nsrcnotes * sizeof(jssrcnote) ==
              (uint8 *)script + size);

    script->compartment = cx->compartment;
#ifdef CHECK_SCRIPT_OWNER
    script->owner = cx->thread;
#endif

    JS_APPEND_LINK(&script->links, &cx->compartment->scripts);
    JS_ASSERT(script->getVersion() == version);
    return script;
}

JSScript *
JSScript::NewScriptFromCG(JSContext *cx, JSCodeGenerator *cg)
{
    uint32 mainLength, prologLength, nsrcnotes, nfixed;
    JSScript *script;
    const char *filename;
    JSFunction *fun;

    /* The counts of indexed things must be checked during code generation. */
    JS_ASSERT(cg->atomList.count <= INDEX_LIMIT);
    JS_ASSERT(cg->objectList.length <= INDEX_LIMIT);
    JS_ASSERT(cg->regexpList.length <= INDEX_LIMIT);

    mainLength = CG_OFFSET(cg);
    prologLength = CG_PROLOG_OFFSET(cg);

    CG_COUNT_FINAL_SRCNOTES(cg, nsrcnotes);
    uint16 nClosedArgs = uint16(cg->closedArgs.length());
    JS_ASSERT(nClosedArgs == cg->closedArgs.length());
    uint16 nClosedVars = uint16(cg->closedVars.length());
    JS_ASSERT(nClosedVars == cg->closedVars.length());
    script = NewScript(cx, prologLength + mainLength, nsrcnotes,
                       cg->atomList.count, cg->objectList.length,
                       cg->upvarList.count, cg->regexpList.length,
                       cg->ntrynotes, cg->constList.length(),
                       cg->globalUses.length(), nClosedArgs, nClosedVars, cg->version());
    if (!script)
        return NULL;

    /* Now that we have script, error control flow must go to label bad. */
    script->main += prologLength;
    memcpy(script->code, CG_PROLOG_BASE(cg), prologLength * sizeof(jsbytecode));
    memcpy(script->main, CG_BASE(cg), mainLength * sizeof(jsbytecode));
    nfixed = cg->inFunction()
             ? cg->bindings.countVars()
             : cg->sharpSlots();
    JS_ASSERT(nfixed < SLOTNO_LIMIT);
    script->nfixed = (uint16) nfixed;
    js_InitAtomMap(cx, &script->atomMap, &cg->atomList);

    filename = cg->parser->tokenStream.getFilename();
    if (filename) {
        script->filename = js_SaveScriptFilename(cx, filename);
        if (!script->filename)
            goto bad;
    }
    script->lineno = cg->firstLine;
    if (script->nfixed + cg->maxStackDepth >= JS_BIT(16)) {
        ReportCompileErrorNumber(cx, CG_TS(cg), NULL, JSREPORT_ERROR, JSMSG_NEED_DIET, "script");
        goto bad;
    }
    script->nslots = script->nfixed + cg->maxStackDepth;
    script->staticLevel = uint16(cg->staticLevel);
    script->principals = cg->parser->principals;
    if (script->principals)
        JSPRINCIPALS_HOLD(cx, script->principals);

    if (!js_FinishTakingSrcNotes(cx, cg, script->notes()))
        goto bad;
    if (cg->ntrynotes != 0)
        js_FinishTakingTryNotes(cg, script->trynotes());
    if (cg->objectList.length != 0)
        cg->objectList.finish(script->objects());
    if (cg->regexpList.length != 0)
        cg->regexpList.finish(script->regexps());
    if (cg->constList.length() != 0)
        cg->constList.finish(script->consts());
    if (cg->flags & TCF_NO_SCRIPT_RVAL)
        script->noScriptRval = true;
    if (cg->hasSharps())
        script->hasSharps = true;
    if (cg->flags & TCF_STRICT_MODE_CODE)
        script->strictModeCode = true;
    if (cg->flags & TCF_COMPILE_N_GO)
        script->compileAndGo = true;
    if (cg->callsEval())
        script->usesEval = true;
    if (cg->flags & TCF_FUN_USES_ARGUMENTS)
        script->usesArguments = true;
    if (cg->flags & TCF_HAS_SINGLETONS)
        script->hasSingletons = true;

    if (cg->upvarList.count != 0) {
        JS_ASSERT(cg->upvarList.count <= cg->upvarMap.length);
        memcpy(script->upvars()->vector, cg->upvarMap.vector,
               cg->upvarList.count * sizeof(uint32));
        cg->upvarList.clear();
        cx->free(cg->upvarMap.vector);
        cg->upvarMap.vector = NULL;
    }

    if (cg->globalUses.length()) {
        memcpy(script->globals()->vector, &cg->globalUses[0],
               cg->globalUses.length() * sizeof(GlobalSlotArray::Entry));
    }

    if (script->nClosedArgs)
        memcpy(script->closedSlots, &cg->closedArgs[0], script->nClosedArgs * sizeof(uint32));
    if (script->nClosedVars) {
        memcpy(&script->closedSlots[script->nClosedArgs], &cg->closedVars[0],
               script->nClosedVars * sizeof(uint32));
    }

    cg->bindings.makeImmutable();
    script->bindings.transfer(cx, &cg->bindings);

    /*
     * We initialize fun->u.script to be the script constructed above
     * so that the debugger has a valid FUN_SCRIPT(fun).
     */
    fun = NULL;
    if (cg->inFunction()) {
        fun = cg->fun();
        JS_ASSERT(fun->isInterpreted());
        JS_ASSERT(!fun->script());
#ifdef DEBUG
        if (JSScript::isValidOffset(script->upvarsOffset))
            JS_ASSERT(script->upvars()->length == script->bindings.countUpvars());
        else
            JS_ASSERT(script->bindings.countUpvars() == 0);
#endif
        fun->u.i.script = script;
#ifdef CHECK_SCRIPT_OWNER
        script->owner = NULL;
#endif
        if (cg->flags & TCF_FUN_HEAVYWEIGHT)
            fun->flags |= JSFUN_HEAVYWEIGHT;
    }

    /* Tell the debugger about this compiled script. */
    js_CallNewScriptHook(cx, script, fun);
#ifdef DEBUG
    {
        jsrefcount newEmptyLive, newLive, newTotal;
        if (script->isEmpty()) {
            newEmptyLive = JS_RUNTIME_METER(cx->runtime, liveEmptyScripts);
            newLive = cx->runtime->liveScripts;
            newTotal =
                JS_RUNTIME_METER(cx->runtime, totalEmptyScripts) + cx->runtime->totalScripts;
        } else {
            newEmptyLive = cx->runtime->liveEmptyScripts;
            newLive = JS_RUNTIME_METER(cx->runtime, liveScripts);
            newTotal =
                cx->runtime->totalEmptyScripts + JS_RUNTIME_METER(cx->runtime, totalScripts);
        }

        jsrefcount oldHigh = cx->runtime->highWaterLiveScripts;
        if (newEmptyLive + newLive > oldHigh) {
            JS_ATOMIC_SET(&cx->runtime->highWaterLiveScripts, newEmptyLive + newLive);
            if (getenv("JS_DUMP_LIVE_SCRIPTS")) {
                fprintf(stderr, "high water script count: %d empty, %d not (total %d)\n",
                        newEmptyLive, newLive, newTotal);
            }
        }
    }
#endif

    return script;

bad:
    js_DestroyScript(cx, script);
    return NULL;
}

JS_FRIEND_API(void)
js_CallNewScriptHook(JSContext *cx, JSScript *script, JSFunction *fun)
{
    JSNewScriptHook hook;

    hook = cx->debugHooks->newScriptHook;
    if (hook) {
        AutoKeepAtoms keep(cx->runtime);
        hook(cx, script->filename, script->lineno, script, fun,
             cx->debugHooks->newScriptHookData);
    }
}

void
js_CallDestroyScriptHook(JSContext *cx, JSScript *script)
{
    JSDestroyScriptHook hook;

    hook = cx->debugHooks->destroyScriptHook;
    if (hook)
        hook(cx, script, cx->debugHooks->destroyScriptHookData);
    JS_ClearScriptTraps(cx, script);
}

static void
DestroyScript(JSContext *cx, JSScript *script)
{
#ifdef DEBUG
    if (script->isEmpty())
        JS_RUNTIME_UNMETER(cx->runtime, liveEmptyScripts);
    else
        JS_RUNTIME_UNMETER(cx->runtime, liveScripts);
#endif

    if (script->principals)
        JSPRINCIPALS_DROP(cx, script->principals);

    if (JS_GSN_CACHE(cx).code == script->code)
        JS_PURGE_GSN_CACHE(cx);

    /*
     * Worry about purging the property cache and any compiled traces related
     * to its bytecode if this script is being destroyed from JS_DestroyScript
     * or equivalent according to a mandatory "New/Destroy" protocol.
     *
     * The GC purges all property caches when regenerating shapes upon shape
     * generator overflow, so no need in that event to purge just the entries
     * for this script.
     *
     * The GC purges trace-JITted code on every GC activation, not just when
     * regenerating shapes, so we don't have to purge fragments if the GC is
     * currently running.
     *
     * JS_THREADSAFE note: The code below purges only the current thread's
     * property cache, so a script not owned by a function or object, which
     * hands off lifetime management for that script to the GC, must be used by
     * only one thread over its lifetime.
     *
     * This should be an API-compatible change, since a script is never safe
     * against premature GC if shared among threads without a rooted object
     * wrapping it to protect the script's mapped atoms against GC. We use
     * script->owner to enforce this requirement via assertions.
     */
#ifdef CHECK_SCRIPT_OWNER
    JS_ASSERT_IF(cx->runtime->gcRunning, !script->owner);
#endif

    /* FIXME: bug 506341; would like to do this only if regenerating shapes. */
    if (!cx->runtime->gcRunning) {
        JS_PROPERTY_CACHE(cx).purgeForScript(cx, script);

#ifdef CHECK_SCRIPT_OWNER
        JS_ASSERT(script->owner == cx->thread);
#endif
    }

#ifdef JS_TRACER
    PurgeScriptFragments(&script->compartment->traceMonitor, script);
#endif

#if defined(JS_METHODJIT)
    mjit::ReleaseScriptCode(cx, script);
#endif
    JS_REMOVE_LINK(&script->links);

    cx->free(script);
}

void
js_DestroyScript(JSContext *cx, JSScript *script)
{
    JS_ASSERT(!cx->runtime->gcRunning);
    js_CallDestroyScriptHook(cx, script);
    DestroyScript(cx, script);
}

void
js_DestroyScriptFromGC(JSContext *cx, JSScript *script)
{
    JS_ASSERT(cx->runtime->gcRunning);
    js_CallDestroyScriptHook(cx, script);
    DestroyScript(cx, script);
}

void
js_DestroyCachedScript(JSContext *cx, JSScript *script)
{
    JS_ASSERT(cx->runtime->gcRunning);
    DestroyScript(cx, script);
}

void
js_TraceScript(JSTracer *trc, JSScript *script)
{
    JSAtomMap *map = &script->atomMap;
    MarkAtomRange(trc, map->length, map->vector, "atomMap");

    if (JSScript::isValidOffset(script->objectsOffset)) {
        JSObjectArray *objarray = script->objects();
        uintN i = objarray->length;
        do {
            --i;
            if (objarray->vector[i]) {
                JS_SET_TRACING_INDEX(trc, "objects", i);
                Mark(trc, objarray->vector[i]);
            }
        } while (i != 0);
    }

    if (JSScript::isValidOffset(script->regexpsOffset)) {
        JSObjectArray *objarray = script->regexps();
        uintN i = objarray->length;
        do {
            --i;
            if (objarray->vector[i]) {
                JS_SET_TRACING_INDEX(trc, "regexps", i);
                Mark(trc, objarray->vector[i]);
            }
        } while (i != 0);
    }

    if (JSScript::isValidOffset(script->constOffset)) {
        JSConstArray *constarray = script->consts();
        MarkValueRange(trc, constarray->length, constarray->vector, "consts");
    }

    if (script->u.object) {
        JS_SET_TRACING_NAME(trc, "object");
        Mark(trc, script->u.object);
    }

    if (IS_GC_MARKING_TRACER(trc) && script->filename)
        js_MarkScriptFilename(script->filename);

    script->bindings.trace(trc);
}

JSObject *
js_NewScriptObject(JSContext *cx, JSScript *script)
{
    AutoScriptRooter root(cx, script);

    JS_ASSERT(!script->u.object);

    JSObject *obj = NewNonFunction<WithProto::Class>(cx, &js_ScriptClass, NULL, NULL);
    if (!obj)
        return NULL;
    obj->setPrivate(script);
    script->u.object = obj;

    /*
     * Clear the object's proto, to avoid entraining stuff. Once we no longer use the parent
     * for security checks, then we can clear the parent, too.
     */
    obj->clearProto();

#ifdef CHECK_SCRIPT_OWNER
    script->owner = NULL;
#endif

    return obj;
}

typedef struct GSNCacheEntry {
    JSDHashEntryHdr     hdr;
    jsbytecode          *pc;
    jssrcnote           *sn;
} GSNCacheEntry;

#define GSN_CACHE_THRESHOLD     100

void
js_PurgeGSNCache(JSGSNCache *cache)
{
    cache->code = NULL;
    if (cache->table.ops) {
        JS_DHashTableFinish(&cache->table);
        cache->table.ops = NULL;
    }
    GSN_CACHE_METER(cache, purges);
}

jssrcnote *
js_GetSrcNoteCached(JSContext *cx, JSScript *script, jsbytecode *pc)
{
    ptrdiff_t target, offset;
    GSNCacheEntry *entry;
    jssrcnote *sn, *result;
    uintN nsrcnotes;


    target = pc - script->code;
    if ((uint32)target >= script->length)
        return NULL;

    if (JS_GSN_CACHE(cx).code == script->code) {
        JS_METER_GSN_CACHE(cx, hits);
        entry = (GSNCacheEntry *)
                JS_DHashTableOperate(&JS_GSN_CACHE(cx).table, pc,
                                     JS_DHASH_LOOKUP);
        return entry->sn;
    }

    JS_METER_GSN_CACHE(cx, misses);
    offset = 0;
    for (sn = script->notes(); ; sn = SN_NEXT(sn)) {
        if (SN_IS_TERMINATOR(sn)) {
            result = NULL;
            break;
        }
        offset += SN_DELTA(sn);
        if (offset == target && SN_IS_GETTABLE(sn)) {
            result = sn;
            break;
        }
    }

    if (JS_GSN_CACHE(cx).code != script->code &&
        script->length >= GSN_CACHE_THRESHOLD) {
        JS_PURGE_GSN_CACHE(cx);
        nsrcnotes = 0;
        for (sn = script->notes(); !SN_IS_TERMINATOR(sn);
             sn = SN_NEXT(sn)) {
            if (SN_IS_GETTABLE(sn))
                ++nsrcnotes;
        }
        if (!JS_DHashTableInit(&JS_GSN_CACHE(cx).table, JS_DHashGetStubOps(),
                               NULL, sizeof(GSNCacheEntry),
                               JS_DHASH_DEFAULT_CAPACITY(nsrcnotes))) {
            JS_GSN_CACHE(cx).table.ops = NULL;
        } else {
            pc = script->code;
            for (sn = script->notes(); !SN_IS_TERMINATOR(sn);
                 sn = SN_NEXT(sn)) {
                pc += SN_DELTA(sn);
                if (SN_IS_GETTABLE(sn)) {
                    entry = (GSNCacheEntry *)
                            JS_DHashTableOperate(&JS_GSN_CACHE(cx).table, pc,
                                                 JS_DHASH_ADD);
                    entry->pc = pc;
                    entry->sn = sn;
                }
            }
            JS_GSN_CACHE(cx).code = script->code;
            JS_METER_GSN_CACHE(cx, fills);
        }
    }

    return result;
}

uintN
js_FramePCToLineNumber(JSContext *cx, JSStackFrame *fp)
{
    return js_PCToLineNumber(cx, fp->script(),
                             fp->hasImacropc() ? fp->imacropc() : fp->pc(cx));
}

uintN
js_PCToLineNumber(JSContext *cx, JSScript *script, jsbytecode *pc)
{
    JSOp op;
    JSFunction *fun;
    uintN lineno;
    ptrdiff_t offset, target;
    jssrcnote *sn;
    JSSrcNoteType type;

    /* Cope with JSStackFrame.pc value prior to entering js_Interpret. */
    if (!pc)
        return 0;

    /*
     * Special case: function definition needs no line number note because
     * the function's script contains its starting line number.
     */
    op = js_GetOpcode(cx, script, pc);
    if (js_CodeSpec[op].format & JOF_INDEXBASE)
        pc += js_CodeSpec[op].length;
    if (*pc == JSOP_DEFFUN) {
        GET_FUNCTION_FROM_BYTECODE(script, pc, 0, fun);
        return fun->u.i.script->lineno;
    }

    /*
     * General case: walk through source notes accumulating their deltas,
     * keeping track of line-number notes, until we pass the note for pc's
     * offset within script->code.
     */
    lineno = script->lineno;
    offset = 0;
    target = pc - script->code;
    for (sn = script->notes(); !SN_IS_TERMINATOR(sn); sn = SN_NEXT(sn)) {
        offset += SN_DELTA(sn);
        type = (JSSrcNoteType) SN_TYPE(sn);
        if (type == SRC_SETLINE) {
            if (offset <= target)
                lineno = (uintN) js_GetSrcNoteOffset(sn, 0);
        } else if (type == SRC_NEWLINE) {
            if (offset <= target)
                lineno++;
        }
        if (offset > target)
            break;
    }
    return lineno;
}

/* The line number limit is the same as the jssrcnote offset limit. */
#define SN_LINE_LIMIT   (SN_3BYTE_OFFSET_FLAG << 16)

jsbytecode *
js_LineNumberToPC(JSScript *script, uintN target)
{
    ptrdiff_t offset, best;
    uintN lineno, bestdiff, diff;
    jssrcnote *sn;
    JSSrcNoteType type;

    offset = 0;
    best = -1;
    lineno = script->lineno;
    bestdiff = SN_LINE_LIMIT;
    for (sn = script->notes(); !SN_IS_TERMINATOR(sn); sn = SN_NEXT(sn)) {
        /*
         * Exact-match only if offset is not in the prolog; otherwise use
         * nearest greater-or-equal line number match.
         */
        if (lineno == target && script->code + offset >= script->main)
            goto out;
        if (lineno >= target) {
            diff = lineno - target;
            if (diff < bestdiff) {
                bestdiff = diff;
                best = offset;
            }
        }
        offset += SN_DELTA(sn);
        type = (JSSrcNoteType) SN_TYPE(sn);
        if (type == SRC_SETLINE) {
            lineno = (uintN) js_GetSrcNoteOffset(sn, 0);
        } else if (type == SRC_NEWLINE) {
            lineno++;
        }
    }
    if (best >= 0)
        offset = best;
out:
    return script->code + offset;
}

JS_FRIEND_API(uintN)
js_GetScriptLineExtent(JSScript *script)
{
    uintN lineno;
    jssrcnote *sn;
    JSSrcNoteType type;

    lineno = script->lineno;
    for (sn = script->notes(); !SN_IS_TERMINATOR(sn); sn = SN_NEXT(sn)) {
        type = (JSSrcNoteType) SN_TYPE(sn);
        if (type == SRC_SETLINE) {
            lineno = (uintN) js_GetSrcNoteOffset(sn, 0);
        } else if (type == SRC_NEWLINE) {
            lineno++;
        }
    }
    return 1 + lineno - script->lineno;
}

class DisablePrincipalsTranscoding {
    JSSecurityCallbacks *callbacks;
    JSPrincipalsTranscoder temp;

  public:
    DisablePrincipalsTranscoding(JSContext *cx)
      : callbacks(JS_GetRuntimeSecurityCallbacks(cx->runtime)),
        temp(NULL)
    {
        if (callbacks) {
            temp = callbacks->principalsTranscoder;
            callbacks->principalsTranscoder = NULL;
        }
    }

    ~DisablePrincipalsTranscoding() {
        if (callbacks)
            callbacks->principalsTranscoder = temp;
    }
};

JSScript *
js_CloneScript(JSContext *cx, JSScript *script)
{
    JS_ASSERT(cx->compartment != script->compartment);
    JS_ASSERT(script->compartment);

    // serialize script
    JSXDRState *w = JS_XDRNewMem(cx, JSXDR_ENCODE);
    if (!w)
        return NULL;

    // we don't want gecko to transcribe our principals for us
    DisablePrincipalsTranscoding disable(cx);

    if (!js_XDRScript(w, &script, NULL)) {
        JS_XDRDestroy(w);
        return NULL;
    }

    uint32 nbytes;
    void *p = JS_XDRMemGetData(w, &nbytes);
    if (!p) {
        JS_XDRDestroy(w);
        return NULL;
    }

    // de-serialize script
    JSXDRState *r = JS_XDRNewMem(cx, JSXDR_DECODE);
    if (!r) {
        JS_XDRDestroy(w);
        return NULL;
    }

    // Hand p off from w to r.  Don't want them to share the data
    // mem, lest they both try to free it in JS_XDRDestroy
    JS_XDRMemSetData(r, p, nbytes);
    JS_XDRMemSetData(w, NULL, 0);

    if (!js_XDRScript(r, &script, NULL))
        return NULL;

    JS_XDRDestroy(r);
    JS_XDRDestroy(w);

    // set the proper principals for the script
    script->principals = script->compartment->principals;
    if (script->principals)
        JSPRINCIPALS_HOLD(cx, script->principals);

    return script;
}

void
JSScript::copyClosedSlotsTo(JSScript *other)
{
    memcpy(other->closedSlots, closedSlots, nClosedArgs + nClosedVars);
}
