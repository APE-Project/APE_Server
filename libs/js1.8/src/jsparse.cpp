/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99:
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
 * JS parser.
 *
 * This is a recursive-descent parser for the JavaScript language specified by
 * "The JavaScript 1.5 Language Specification".  It uses lexical and semantic
 * feedback to disambiguate non-LL(1) structures.  It generates trees of nodes
 * induced by the recursive parsing (not precise syntax trees, see jsparse.h).
 * After tree construction, it rewrites trees to fold constants and evaluate
 * compile-time expressions.  Finally, it calls js_EmitTree (see jsemit.h) to
 * generate bytecode.
 *
 * This parser attempts no error recovery.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "jstypes.h"
#include "jsstdint.h"
#include "jsarena.h" /* Added by JSIFY */
#include "jsutil.h" /* Added by JSIFY */
#include "jsapi.h"
#include "jsarray.h"
#include "jsatom.h"
#include "jscntxt.h"
#include "jsversion.h"
#include "jsemit.h"
#include "jsfun.h"
#include "jsinterp.h"
#include "jsiter.h"
#include "jslock.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jsopcode.h"
#include "jsparse.h"
#include "jsscan.h"
#include "jsscope.h"
#include "jsscript.h"
#include "jsstr.h"
#include "jsstaticcheck.h"
#include "jslibmath.h"
#include "jsvector.h"

#if JS_HAS_XML_SUPPORT
#include "jsxml.h"
#endif

#if JS_HAS_DESTRUCTURING
#include "jsdhash.h"
#endif

/*
 * Asserts to verify assumptions behind pn_ macros.
 */
#define pn_offsetof(m)  offsetof(JSParseNode, m)

JS_STATIC_ASSERT(pn_offsetof(pn_link) == pn_offsetof(dn_uses));
JS_STATIC_ASSERT(pn_offsetof(pn_u.name.atom) == pn_offsetof(pn_u.apair.atom));

#undef pn_offsetof

/*
 * JS parsers, from lowest to highest precedence.
 *
 * Each parser takes a context, a token stream, and a tree context struct.
 * Each returns a parse node tree or null on error.
 */

typedef JSParseNode *
JSParser(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc);

typedef JSParseNode *
JSVariablesParser(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc,
                  bool inLetHead);

typedef JSParseNode *
JSMemberParser(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc,
               JSBool allowCallSyntax);

typedef JSParseNode *
JSPrimaryParser(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc,
                JSTokenType tt, JSBool afterDot);

typedef JSParseNode *
JSParenParser(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc,
              JSParseNode *pn1, JSBool *genexp);

static JSParser FunctionStmt;
static JSParser FunctionExpr;
static JSParser Statements;
static JSParser Statement;
static JSVariablesParser Variables;
static JSParser Expr;
static JSParser AssignExpr;
static JSParser CondExpr;
static JSParser OrExpr;
static JSParser AndExpr;
static JSParser BitOrExpr;
static JSParser BitXorExpr;
static JSParser BitAndExpr;
static JSParser EqExpr;
static JSParser RelExpr;
static JSParser ShiftExpr;
static JSParser AddExpr;
static JSParser MulExpr;
static JSParser UnaryExpr;
static JSMemberParser  MemberExpr;
static JSPrimaryParser PrimaryExpr;
static JSParenParser   ParenExpr;

/*
 * Insist that the next token be of type tt, or report errno and return null.
 * NB: this macro uses cx and ts from its lexical environment.
 */
#define MUST_MATCH_TOKEN(tt, errno)                                           \
    JS_BEGIN_MACRO                                                            \
        if (js_GetToken(cx, ts) != tt) {                                      \
            js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR, errno); \
            return NULL;                                                      \
        }                                                                     \
    JS_END_MACRO

#ifdef METER_PARSENODES
static uint32 parsenodes = 0;
static uint32 maxparsenodes = 0;
static uint32 recyclednodes = 0;
#endif

void
JSParseNode::become(JSParseNode *pn2)
{
    JS_ASSERT(!pn_defn);
    JS_ASSERT(!pn2->pn_defn);

    JS_ASSERT(!pn_used);
    if (pn2->pn_used) {
        JSParseNode **pnup = &pn2->pn_lexdef->dn_uses;
        while (*pnup != pn2)
            pnup = &(*pnup)->pn_link;
        *pnup = this;
        pn_link = pn2->pn_link;
        pn_used = true;
        pn2->pn_link = NULL;
        pn2->pn_used = false;
    }

    /* If this is a function node fix up the pn_funbox->node back-pointer. */
    if (PN_TYPE(pn2) == TOK_FUNCTION && pn2->pn_arity == PN_FUNC)
        pn2->pn_funbox->node = this;

    pn_type = pn2->pn_type;
    pn_op = pn2->pn_op;
    pn_arity = pn2->pn_arity;
    pn_parens = pn2->pn_parens;
    pn_u = pn2->pn_u;
    pn2->clear();
}

void
JSParseNode::clear()
{
    pn_type = TOK_EOF;
    pn_op = JSOP_NOP;
    pn_used = pn_defn = false;
    pn_arity = PN_NULLARY;
    pn_parens = false;
}

bool
JSCompiler::init(const jschar *base, size_t length,
                 FILE *fp, const char *filename, uintN lineno)
{
    JSContext *cx = context;

    tempPoolMark = JS_ARENA_MARK(&cx->tempPool);
    if (!tokenStream.init(cx, base, length, fp, filename, lineno)) {
        JS_ARENA_RELEASE(&cx->tempPool, tempPoolMark);
        return false;
    }

    /* Root atoms and objects allocated for the parsed tree. */
    JS_KEEP_ATOMS(cx->runtime);
    JS_PUSH_TEMP_ROOT_COMPILER(cx, this, &tempRoot);
    return true;
}

JSCompiler::~JSCompiler()
{
    JSContext *cx = context;

    if (principals)
        JSPRINCIPALS_DROP(cx, principals);
    JS_ASSERT(tempRoot.u.compiler == this);
    JS_POP_TEMP_ROOT(cx, &tempRoot);
    JS_UNKEEP_ATOMS(cx->runtime);
    tokenStream.close(cx);
    JS_ARENA_RELEASE(&cx->tempPool, tempPoolMark);
}

void
JSCompiler::setPrincipals(JSPrincipals *prin)
{
    JS_ASSERT(!principals);
    if (prin)
        JSPRINCIPALS_HOLD(context, prin);
    principals = prin;
}

JSObjectBox *
JSCompiler::newObjectBox(JSObject *obj)
{
    JS_ASSERT(obj);

    /*
     * We use JSContext.tempPool to allocate parsed objects and place them on
     * a list in JSTokenStream to ensure GC safety. Thus the tempPool arenas
     * containing the entries must be alive until we are done with scanning,
     * parsing and code generation for the whole script or top-level function.
     */
    JSObjectBox *objbox;
    JS_ARENA_ALLOCATE_TYPE(objbox, JSObjectBox, &context->tempPool);
    if (!objbox) {
        js_ReportOutOfScriptQuota(context);
        return NULL;
    }
    objbox->traceLink = traceListHead;
    traceListHead = objbox;
    objbox->emitLink = NULL;
    objbox->object = obj;
    return objbox;
}

JSFunctionBox *
JSCompiler::newFunctionBox(JSObject *obj, JSParseNode *fn, JSTreeContext *tc)
{
    JS_ASSERT(obj);
    JS_ASSERT(HAS_FUNCTION_CLASS(obj));

    /*
     * We use JSContext.tempPool to allocate parsed objects and place them on
     * a list in JSTokenStream to ensure GC safety. Thus the tempPool arenas
     * containing the entries must be alive until we are done with scanning,
     * parsing and code generation for the whole script or top-level function.
     */
    JSFunctionBox *funbox;
    JS_ARENA_ALLOCATE_TYPE(funbox, JSFunctionBox, &context->tempPool);
    if (!funbox) {
        js_ReportOutOfScriptQuota(context);
        return NULL;
    }
    funbox->traceLink = traceListHead;
    traceListHead = funbox;
    funbox->emitLink = NULL;
    funbox->object = obj;
    funbox->node = fn;
    funbox->siblings = tc->functionList;
    tc->functionList = funbox;
    ++tc->compiler->functionCount;
    funbox->kids = NULL;
    funbox->parent = tc->funbox;
    funbox->queued = false;
    funbox->inLoop = false;
    for (JSStmtInfo *stmt = tc->topStmt; stmt; stmt = stmt->down) {
        if (STMT_IS_LOOP(stmt)) {
            funbox->inLoop = true;
            break;
        }
    }
    funbox->level = tc->staticLevel;
    funbox->tcflags = TCF_IN_FUNCTION | (tc->flags & TCF_COMPILE_N_GO);
    return funbox;
}

void
JSCompiler::trace(JSTracer *trc)
{
    JSObjectBox *objbox;

    JS_ASSERT(tempRoot.u.compiler == this);
    objbox = traceListHead;
    while (objbox) {
        JS_CALL_OBJECT_TRACER(trc, objbox->object, "parser.object");
        objbox = objbox->traceLink;
    }
}

static void
UnlinkFunctionBoxes(JSParseNode *pn, JSTreeContext *tc);

static void
UnlinkFunctionBox(JSParseNode *pn, JSTreeContext *tc)
{
    JSFunctionBox *funbox = pn->pn_funbox;
    if (funbox) {
        JS_ASSERT(funbox->node == pn);
        funbox->node = NULL;

        JSFunctionBox **funboxp = &tc->functionList;
        while (*funboxp) {
            if (*funboxp == funbox) {
                *funboxp = funbox->siblings;
                break;
            }
            funboxp = &(*funboxp)->siblings;
        }

        uint32 oldflags = tc->flags;
        JSFunctionBox *oldlist = tc->functionList;

        tc->flags = funbox->tcflags;
        tc->functionList = funbox->kids;
        UnlinkFunctionBoxes(pn->pn_body, tc);
        funbox->kids = tc->functionList;
        tc->flags = oldflags;
        tc->functionList = oldlist;

        // FIXME: use a funbox freelist (consolidate aleFreeList and nodeList).
        pn->pn_funbox = NULL;
    }
}

static void
UnlinkFunctionBoxes(JSParseNode *pn, JSTreeContext *tc)
{
    if (pn) {
        switch (pn->pn_arity) {
          case PN_NULLARY:
            return;
          case PN_UNARY:
            UnlinkFunctionBoxes(pn->pn_kid, tc);
            return;
          case PN_BINARY:
            UnlinkFunctionBoxes(pn->pn_left, tc);
            UnlinkFunctionBoxes(pn->pn_right, tc);
            return;
          case PN_TERNARY:
            UnlinkFunctionBoxes(pn->pn_kid1, tc);
            UnlinkFunctionBoxes(pn->pn_kid2, tc);
            UnlinkFunctionBoxes(pn->pn_kid3, tc);
            return;
          case PN_LIST:
            for (JSParseNode *pn2 = pn->pn_head; pn2; pn2 = pn2->pn_next)
                UnlinkFunctionBoxes(pn2, tc);
            return;
          case PN_FUNC:
            UnlinkFunctionBox(pn, tc);
            return;
          case PN_NAME:
            UnlinkFunctionBoxes(pn->maybeExpr(), tc);
            return;
          case PN_NAMESET:
            UnlinkFunctionBoxes(pn->pn_tree, tc);
        }
    }
}

static void
RecycleFuncNameKids(JSParseNode *pn, JSTreeContext *tc);

static JSParseNode *
RecycleTree(JSParseNode *pn, JSTreeContext *tc)
{
    JSParseNode *next, **head;

    if (!pn)
        return NULL;

    /* Catch back-to-back dup recycles. */
    JS_ASSERT(pn != tc->compiler->nodeList);
    next = pn->pn_next;
    if (pn->pn_used || pn->pn_defn) {
        /*
         * JSAtomLists own definition nodes along with their used-node chains.
         * Defer recycling such nodes until we unwind to top level to avoid
         * linkage overhead or (alternatively) unlinking runtime complexity.
         * Yes, this means dead code can contribute to static analysis results!
         *
         * Do recycle kids here, since they are no longer needed.
         */
        pn->pn_next = NULL;
        RecycleFuncNameKids(pn, tc);
    } else {
        UnlinkFunctionBoxes(pn, tc);
        head = &tc->compiler->nodeList;
        pn->pn_next = *head;
        *head = pn;
#ifdef METER_PARSENODES
        recyclednodes++;
#endif
    }
    return next;
}

static void
RecycleFuncNameKids(JSParseNode *pn, JSTreeContext *tc)
{
    switch (pn->pn_arity) {
      case PN_FUNC:
        UnlinkFunctionBox(pn, tc);
        /* FALL THROUGH */

      case PN_NAME:
        /*
         * Only a definition node might have a non-null strong pn_expr link
         * to recycle, but we test !pn_used to handle PN_FUNC fall through.
         * Every node with the pn_used flag set has a non-null pn_lexdef
         * weak reference to its definition node.
         */
        if (!pn->pn_used && pn->pn_expr) {
            RecycleTree(pn->pn_expr, tc);
            pn->pn_expr = NULL;
        }
        break;

      default:
        JS_ASSERT(PN_TYPE(pn) == TOK_FUNCTION);
    }
}

static JSParseNode *
NewOrRecycledNode(JSTreeContext *tc)
{
    JSParseNode *pn, *pn2;

    pn = tc->compiler->nodeList;
    if (!pn) {
        JSContext *cx = tc->compiler->context;

        JS_ARENA_ALLOCATE_TYPE(pn, JSParseNode, &cx->tempPool);
        if (!pn)
            js_ReportOutOfScriptQuota(cx);
    } else {
        tc->compiler->nodeList = pn->pn_next;

        /* Recycle immediate descendents only, to save work and working set. */
        switch (pn->pn_arity) {
          case PN_FUNC:
            RecycleTree(pn->pn_body, tc);
            break;
          case PN_LIST:
            pn2 = pn->pn_head;
            if (pn2) {
                while (pn2 && !pn2->pn_used && !pn2->pn_defn)
                    pn2 = pn2->pn_next;
                if (pn2) {
                    pn2 = pn->pn_head;
                    do {
                        pn2 = RecycleTree(pn2, tc);
                    } while (pn2);
                } else {
                    *pn->pn_tail = tc->compiler->nodeList;
                    tc->compiler->nodeList = pn->pn_head;
#ifdef METER_PARSENODES
                    recyclednodes += pn->pn_count;
#endif
                    break;
                }
            }
            break;
          case PN_TERNARY:
            RecycleTree(pn->pn_kid1, tc);
            RecycleTree(pn->pn_kid2, tc);
            RecycleTree(pn->pn_kid3, tc);
            break;
          case PN_BINARY:
            if (pn->pn_left != pn->pn_right)
                RecycleTree(pn->pn_left, tc);
            RecycleTree(pn->pn_right, tc);
            break;
          case PN_UNARY:
            RecycleTree(pn->pn_kid, tc);
            break;
          case PN_NAME:
            if (!pn->pn_used)
                RecycleTree(pn->pn_expr, tc);
            break;
          case PN_NULLARY:
            break;
        }
    }
    if (pn) {
#ifdef METER_PARSENODES
        parsenodes++;
        if (parsenodes - recyclednodes > maxparsenodes)
            maxparsenodes = parsenodes - recyclednodes;
#endif
        pn->pn_used = pn->pn_defn = false;
        memset(&pn->pn_u, 0, sizeof pn->pn_u);
        pn->pn_next = NULL;
    }
    return pn;
}

static inline void
InitParseNode(JSParseNode *pn, JSTokenType type, JSOp op, JSParseNodeArity arity)
{
    pn->pn_type = type;
    pn->pn_op = op;
    pn->pn_arity = arity;
    pn->pn_parens = false;
    JS_ASSERT(!pn->pn_used);
    JS_ASSERT(!pn->pn_defn);
    pn->pn_next = pn->pn_link = NULL;
}

/*
 * Allocate a JSParseNode from tc's node freelist or, failing that, from cx's
 * temporary arena.
 */
static JSParseNode *
NewParseNode(JSParseNodeArity arity, JSTreeContext *tc)
{
    JSParseNode *pn;
    JSToken *tp;

    pn = NewOrRecycledNode(tc);
    if (!pn)
        return NULL;
    tp = &CURRENT_TOKEN(&tc->compiler->tokenStream);
    InitParseNode(pn, tp->type, JSOP_NOP, arity);
    pn->pn_pos = tp->pos;
    return pn;
}

static inline void
InitNameNodeCommon(JSParseNode *pn, JSTreeContext *tc)
{
    pn->pn_expr = NULL;
    pn->pn_cookie = FREE_UPVAR_COOKIE;
    pn->pn_dflags = tc->atTopLevel() ? PND_TOPLEVEL : 0;
    if (!tc->topStmt || tc->topStmt->type == STMT_BLOCK)
        pn->pn_dflags |= PND_BLOCKCHILD;
    pn->pn_blockid = tc->blockid();
}

static JSParseNode *
NewNameNode(JSContext *cx, JSAtom *atom, JSTreeContext *tc)
{
    JSParseNode *pn;

    pn = NewParseNode(PN_NAME, tc);
    if (pn) {
        pn->pn_atom = atom;
        InitNameNodeCommon(pn, tc);
    }
    return pn;
}

static JSParseNode *
NewBinary(JSTokenType tt, JSOp op, JSParseNode *left, JSParseNode *right,
          JSTreeContext *tc)
{
    JSParseNode *pn, *pn1, *pn2;

    if (!left || !right)
        return NULL;

    /*
     * Flatten a left-associative (left-heavy) tree of a given operator into
     * a list, to reduce js_FoldConstants and js_EmitTree recursion.
     */
    if (PN_TYPE(left) == tt &&
        PN_OP(left) == op &&
        (js_CodeSpec[op].format & JOF_LEFTASSOC)) {
        if (left->pn_arity != PN_LIST) {
            pn1 = left->pn_left, pn2 = left->pn_right;
            left->pn_arity = PN_LIST;
            left->pn_parens = false;
            left->initList(pn1);
            left->append(pn2);
            if (tt == TOK_PLUS) {
                if (pn1->pn_type == TOK_STRING)
                    left->pn_xflags |= PNX_STRCAT;
                else if (pn1->pn_type != TOK_NUMBER)
                    left->pn_xflags |= PNX_CANTFOLD;
                if (pn2->pn_type == TOK_STRING)
                    left->pn_xflags |= PNX_STRCAT;
                else if (pn2->pn_type != TOK_NUMBER)
                    left->pn_xflags |= PNX_CANTFOLD;
            }
        }
        left->append(right);
        left->pn_pos.end = right->pn_pos.end;
        if (tt == TOK_PLUS) {
            if (right->pn_type == TOK_STRING)
                left->pn_xflags |= PNX_STRCAT;
            else if (right->pn_type != TOK_NUMBER)
                left->pn_xflags |= PNX_CANTFOLD;
        }
        return left;
    }

    /*
     * Fold constant addition immediately, to conserve node space and, what's
     * more, so js_FoldConstants never sees mixed addition and concatenation
     * operations with more than one leading non-string operand in a PN_LIST
     * generated for expressions such as 1 + 2 + "pt" (which should evaluate
     * to "3pt", not "12pt").
     */
    if (tt == TOK_PLUS &&
        left->pn_type == TOK_NUMBER &&
        right->pn_type == TOK_NUMBER) {
        left->pn_dval += right->pn_dval;
        left->pn_pos.end = right->pn_pos.end;
        RecycleTree(right, tc);
        return left;
    }

    pn = NewOrRecycledNode(tc);
    if (!pn)
        return NULL;
    InitParseNode(pn, tt, op, PN_BINARY);
    pn->pn_pos.begin = left->pn_pos.begin;
    pn->pn_pos.end = right->pn_pos.end;
    pn->pn_left = left;
    pn->pn_right = right;
    return pn;
}

#if JS_HAS_GETTER_SETTER
static JSTokenType
CheckGetterOrSetter(JSContext *cx, JSTokenStream *ts, JSTokenType tt)
{
    JSAtom *atom;
    JSRuntime *rt;
    JSOp op;
    const char *name;

    JS_ASSERT(CURRENT_TOKEN(ts).type == TOK_NAME);
    atom = CURRENT_TOKEN(ts).t_atom;
    rt = cx->runtime;
    if (atom == rt->atomState.getterAtom)
        op = JSOP_GETTER;
    else if (atom == rt->atomState.setterAtom)
        op = JSOP_SETTER;
    else
        return TOK_NAME;
    if (js_PeekTokenSameLine(cx, ts) != tt)
        return TOK_NAME;
    (void) js_GetToken(cx, ts);
    if (CURRENT_TOKEN(ts).t_op != JSOP_NOP) {
        js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                    JSMSG_BAD_GETTER_OR_SETTER,
                                    (op == JSOP_GETTER)
                                    ? js_getter_str
                                    : js_setter_str);
        return TOK_ERROR;
    }
    CURRENT_TOKEN(ts).t_op = op;
    if (JS_HAS_STRICT_OPTION(cx)) {
        name = js_AtomToPrintableString(cx, atom);
        if (!name ||
            !js_ReportCompileErrorNumber(cx, ts, NULL,
                                         JSREPORT_WARNING | JSREPORT_STRICT,
                                         JSMSG_DEPRECATED_USAGE,
                                         name)) {
            return TOK_ERROR;
        }
    }
    return tt;
}
#endif

static bool
GenerateBlockId(JSTreeContext *tc, uint32& blockid)
{
    if (tc->blockidGen == JS_BIT(20)) {
        JS_ReportErrorNumber(tc->compiler->context, js_GetErrorMessage, NULL,
                             JSMSG_NEED_DIET, "program");
        return false;
    }
    blockid = tc->blockidGen++;
    return true;
}

static bool
GenerateBlockIdForStmtNode(JSParseNode *pn, JSTreeContext *tc)
{
    JS_ASSERT(tc->topStmt);
    JS_ASSERT(STMT_MAYBE_SCOPE(tc->topStmt));
    JS_ASSERT(pn->pn_type == TOK_LC || pn->pn_type == TOK_LEXICALSCOPE);
    if (!GenerateBlockId(tc, tc->topStmt->blockid))
        return false;
    pn->pn_blockid = tc->topStmt->blockid;
    return true;
}

/*
 * Parse a top-level JS script.
 */
JSParseNode *
JSCompiler::parse(JSObject *chain)
{
    /*
     * Protect atoms from being collected by a GC activation, which might
     * - nest on this thread due to out of memory (the so-called "last ditch"
     *   GC attempted within js_NewGCThing), or
     * - run for any reason on another thread if this thread is suspended on
     *   an object lock before it finishes generating bytecode into a script
     *   protected from the GC by a root or a stack frame reference.
     */
    JSTreeContext tc(this);
    tc.scopeChain = chain;
    if (!GenerateBlockId(&tc, tc.bodyid))
        return NULL;

    JSParseNode *pn = Statements(context, TS(this), &tc);
    if (pn) {
        if (!js_MatchToken(context, TS(this), TOK_EOF)) {
            js_ReportCompileErrorNumber(context, TS(this), NULL, JSREPORT_ERROR,
                                        JSMSG_SYNTAX_ERROR);
            pn = NULL;
        } else {
            if (!js_FoldConstants(context, pn, &tc))
                pn = NULL;
        }
    }
    return pn;
}

JS_STATIC_ASSERT(FREE_STATIC_LEVEL == JS_BITMASK(JSFB_LEVEL_BITS));

static inline bool
SetStaticLevel(JSTreeContext *tc, uintN staticLevel)
{
    /*
     * Reserve FREE_STATIC_LEVEL (0xffff) in order to reserve FREE_UPVAR_COOKIE
     * (0xffffffff) and other cookies with that level.
     *
     * This is a lot simpler than error-checking every MAKE_UPVAR_COOKIE, and
     * practically speaking it leaves more than enough room for upvars. In fact
     * we might want to split cookie fields giving fewer bits for skip and more
     * for slot, but only based on evidence.
     */
    if (staticLevel >= FREE_STATIC_LEVEL) {
        JS_ReportErrorNumber(tc->compiler->context, js_GetErrorMessage, NULL,
                             JSMSG_TOO_DEEP, js_function_str);
        return false;
    }
    tc->staticLevel = staticLevel;
    return true;
}

/*
 * Compile a top-level script.
 */
JSScript *
JSCompiler::compileScript(JSContext *cx, JSObject *scopeChain, JSStackFrame *callerFrame,
                          JSPrincipals *principals, uint32 tcflags,
                          const jschar *chars, size_t length,
                          FILE *file, const char *filename, uintN lineno,
                          JSString *source /* = NULL */,
                          unsigned staticLevel /* = 0 */)
{
    JSCompiler jsc(cx, principals, callerFrame);
    JSArenaPool codePool, notePool;
    JSTokenType tt;
    JSParseNode *pn;
    uint32 scriptGlobals;
    JSScript *script;
#ifdef METER_PARSENODES
    void *sbrk(ptrdiff_t), *before = sbrk(0);
#endif

    JS_ASSERT(!(tcflags & ~(TCF_COMPILE_N_GO | TCF_NO_SCRIPT_RVAL)));

    /*
     * The scripted callerFrame can only be given for compile-and-go scripts
     * and non-zero static level requires callerFrame.
     */
    JS_ASSERT_IF(callerFrame, tcflags & TCF_COMPILE_N_GO);
    JS_ASSERT_IF(staticLevel != 0, callerFrame);

    if (!jsc.init(chars, length, file, filename, lineno))
        return NULL;

    JS_INIT_ARENA_POOL(&codePool, "code", 1024, sizeof(jsbytecode),
                       &cx->scriptStackQuota);
    JS_INIT_ARENA_POOL(&notePool, "note", 1024, sizeof(jssrcnote),
                       &cx->scriptStackQuota);

    JSCodeGenerator cg(&jsc, &codePool, &notePool, jsc.tokenStream.lineno);

    MUST_FLOW_THROUGH("out");

    /* Null script early in case of error, to reduce our code footprint. */
    script = NULL;

    cg.flags |= tcflags;
    cg.scopeChain = scopeChain;
    if (!SetStaticLevel(&cg, staticLevel))
        goto out;

    /*
     * If funbox is non-null after we create the new script, callerFrame->fun
     * was saved in the 0th object table entry.
     */
    JSObjectBox *funbox;
    funbox = NULL;

    if (tcflags & TCF_COMPILE_N_GO) {
        if (source) {
            /*
             * Save eval program source in script->atomMap.vector[0] for the
             * eval cache (see obj_eval in jsobj.cpp).
             */
            JSAtom *atom = js_AtomizeString(cx, source, 0);
            if (!atom || !cg.atomList.add(&jsc, atom))
                goto out;
        }

        if (callerFrame && callerFrame->fun) {
            /*
             * An eval script in a caller frame needs to have its enclosing
             * function captured in case it refers to an upvar, and someone
             * wishes to decompile it while it's running.
             */
            funbox = jsc.newObjectBox(FUN_OBJECT(callerFrame->fun));
            if (!funbox)
                goto out;
            funbox->emitLink = cg.objectList.lastbox;
            cg.objectList.lastbox = funbox;
            cg.objectList.length++;
        }
    }

    /*
     * Inline Statements to emit as we go to save AST space. We must generate
     * our script-body blockid since we aren't calling Statements.
     */
    uint32 bodyid;
    if (!GenerateBlockId(&cg, bodyid))
        goto out;
    cg.bodyid = bodyid;

#if JS_HAS_XML_SUPPORT
    pn = NULL;
    bool onlyXML;
    onlyXML = true;
#endif

    CG_SWITCH_TO_PROLOG(&cg);
    if (js_Emit1(cx, &cg, JSOP_TRACE) < 0)
        goto out;
    CG_SWITCH_TO_MAIN(&cg);

    for (;;) {
        jsc.tokenStream.flags |= TSF_OPERAND;
        tt = js_PeekToken(cx, &jsc.tokenStream);
        jsc.tokenStream.flags &= ~TSF_OPERAND;
        if (tt <= TOK_EOF) {
            if (tt == TOK_EOF)
                break;
            JS_ASSERT(tt == TOK_ERROR);
            goto out;
        }

        pn = Statement(cx, &jsc.tokenStream, &cg);
        if (!pn)
            goto out;
        JS_ASSERT(!cg.blockNode);

        if (!js_FoldConstants(cx, pn, &cg))
            goto out;

        if (cg.functionList) {
            if (!jsc.analyzeFunctions(cg.functionList, cg.flags))
                goto out;
            cg.functionList = NULL;
        }

        if (!js_EmitTree(cx, &cg, pn))
            goto out;
#if JS_HAS_XML_SUPPORT
        if (PN_TYPE(pn) != TOK_SEMI ||
            !pn->pn_kid ||
            !TREE_TYPE_IS_XML(PN_TYPE(pn->pn_kid))) {
            onlyXML = false;
        }
#endif
        RecycleTree(pn, &cg);
    }

#if JS_HAS_XML_SUPPORT
    /*
     * Prevent XML data theft via <script src="http://victim.com/foo.xml">.
     * For background, see:
     *
     * https://bugzilla.mozilla.org/show_bug.cgi?id=336551
     */
    if (pn && onlyXML && (tcflags & TCF_NO_SCRIPT_RVAL)) {
        js_ReportCompileErrorNumber(cx, &jsc.tokenStream, NULL, JSREPORT_ERROR,
                                    JSMSG_XML_WHOLE_PROGRAM);
        goto out;
    }
#endif

    /*
     * Global variables and regexps share the index space with locals. Due to
     * incremental code generation we need to patch the bytecode to adjust the
     * local references to skip the globals.
     */
    scriptGlobals = cg.ngvars + cg.regexpList.length;
    if (scriptGlobals != 0 || cg.hasSharps()) {
        jsbytecode *code, *end;
        JSOp op;
        const JSCodeSpec *cs;
        uintN len, slot;

        if (scriptGlobals >= SLOTNO_LIMIT)
            goto too_many_slots;
        code = CG_BASE(&cg);
        for (end = code + CG_OFFSET(&cg); code != end; code += len) {
            JS_ASSERT(code < end);
            op = (JSOp) *code;
            cs = &js_CodeSpec[op];
            len = (cs->length > 0)
                  ? (uintN) cs->length
                  : js_GetVariableBytecodeLength(code);
            if ((cs->format & JOF_SHARPSLOT) ||
                JOF_TYPE(cs->format) == JOF_LOCAL ||
                (JOF_TYPE(cs->format) == JOF_SLOTATOM)) {
                /*
                 * JSOP_GETARGPROP also has JOF_SLOTATOM type, but it may be
                 * emitted only for a function.
                 */
                JS_ASSERT_IF(!(cs->format & JOF_SHARPSLOT),
                             (JOF_TYPE(cs->format) == JOF_SLOTATOM) ==
                             (op == JSOP_GETLOCALPROP));
                slot = GET_SLOTNO(code);
                slot += scriptGlobals;
                if (!(cs->format & JOF_SHARPSLOT))
                    slot += cg.sharpSlots();
                if (slot >= SLOTNO_LIMIT)
                    goto too_many_slots;
                SET_SLOTNO(code, slot);
            }
        }
    }

#ifdef METER_PARSENODES
    printf("Parser growth: %d (%u nodes, %u max, %u unrecycled)\n",
           (char *)sbrk(0) - (char *)before,
           parsenodes,
           maxparsenodes,
           parsenodes - recyclednodes);
    before = sbrk(0);
#endif

    /*
     * Nowadays the threaded interpreter needs a stop instruction, so we
     * do have to emit that here.
     */
    if (js_Emit1(cx, &cg, JSOP_STOP) < 0)
        goto out;
#ifdef METER_PARSENODES
    printf("Code-gen growth: %d (%u bytecodes, %u srcnotes)\n",
           (char *)sbrk(0) - (char *)before, CG_OFFSET(&cg), cg.noteCount);
#endif
#ifdef JS_ARENAMETER
    JS_DumpArenaStats(stdout);
#endif
    script = js_NewScriptFromCG(cx, &cg);
    if (script && funbox)
        script->savedCallerFun = true;

#ifdef JS_SCOPE_DEPTH_METER
    if (script) {
        JSObject *obj = scopeChain;
        uintN depth = 1;
        while ((obj = OBJ_GET_PARENT(cx, obj)) != NULL)
            ++depth;
        JS_BASIC_STATS_ACCUM(&cx->runtime->hostenvScopeDepthStats, depth);
    }
#endif

  out:
    JS_FinishArenaPool(&codePool);
    JS_FinishArenaPool(&notePool);
    return script;

  too_many_slots:
    js_ReportCompileErrorNumber(cx, &jsc.tokenStream, NULL,
                                JSREPORT_ERROR, JSMSG_TOO_MANY_LOCALS);
    script = NULL;
    goto out;
}

/*
 * Insist on a final return before control flows out of pn.  Try to be a bit
 * smart about loops: do {...; return e2;} while(0) at the end of a function
 * that contains an early return e1 will get a strict warning.  Similarly for
 * iloops: while (true){...} is treated as though ... returns.
 */
#define ENDS_IN_OTHER   0
#define ENDS_IN_RETURN  1
#define ENDS_IN_BREAK   2

static int
HasFinalReturn(JSParseNode *pn)
{
    JSParseNode *pn2, *pn3;
    uintN rv, rv2, hasDefault;

    switch (pn->pn_type) {
      case TOK_LC:
        if (!pn->pn_head)
            return ENDS_IN_OTHER;
        return HasFinalReturn(pn->last());

      case TOK_IF:
        if (!pn->pn_kid3)
            return ENDS_IN_OTHER;
        return HasFinalReturn(pn->pn_kid2) & HasFinalReturn(pn->pn_kid3);

      case TOK_WHILE:
        pn2 = pn->pn_left;
        if (pn2->pn_type == TOK_PRIMARY && pn2->pn_op == JSOP_TRUE)
            return ENDS_IN_RETURN;
        if (pn2->pn_type == TOK_NUMBER && pn2->pn_dval)
            return ENDS_IN_RETURN;
        return ENDS_IN_OTHER;

      case TOK_DO:
        pn2 = pn->pn_right;
        if (pn2->pn_type == TOK_PRIMARY) {
            if (pn2->pn_op == JSOP_FALSE)
                return HasFinalReturn(pn->pn_left);
            if (pn2->pn_op == JSOP_TRUE)
                return ENDS_IN_RETURN;
        }
        if (pn2->pn_type == TOK_NUMBER) {
            if (pn2->pn_dval == 0)
                return HasFinalReturn(pn->pn_left);
            return ENDS_IN_RETURN;
        }
        return ENDS_IN_OTHER;

      case TOK_FOR:
        pn2 = pn->pn_left;
        if (pn2->pn_arity == PN_TERNARY && !pn2->pn_kid2)
            return ENDS_IN_RETURN;
        return ENDS_IN_OTHER;

      case TOK_SWITCH:
        rv = ENDS_IN_RETURN;
        hasDefault = ENDS_IN_OTHER;
        pn2 = pn->pn_right;
        if (pn2->pn_type == TOK_LEXICALSCOPE)
            pn2 = pn2->expr();
        for (pn2 = pn2->pn_head; rv && pn2; pn2 = pn2->pn_next) {
            if (pn2->pn_type == TOK_DEFAULT)
                hasDefault = ENDS_IN_RETURN;
            pn3 = pn2->pn_right;
            JS_ASSERT(pn3->pn_type == TOK_LC);
            if (pn3->pn_head) {
                rv2 = HasFinalReturn(pn3->last());
                if (rv2 == ENDS_IN_OTHER && pn2->pn_next)
                    /* Falling through to next case or default. */;
                else
                    rv &= rv2;
            }
        }
        /* If a final switch has no default case, we judge it harshly. */
        rv &= hasDefault;
        return rv;

      case TOK_BREAK:
        return ENDS_IN_BREAK;

      case TOK_WITH:
        return HasFinalReturn(pn->pn_right);

      case TOK_RETURN:
        return ENDS_IN_RETURN;

      case TOK_COLON:
      case TOK_LEXICALSCOPE:
        return HasFinalReturn(pn->expr());

      case TOK_THROW:
        return ENDS_IN_RETURN;

      case TOK_TRY:
        /* If we have a finally block that returns, we are done. */
        if (pn->pn_kid3) {
            rv = HasFinalReturn(pn->pn_kid3);
            if (rv == ENDS_IN_RETURN)
                return rv;
        }

        /* Else check the try block and any and all catch statements. */
        rv = HasFinalReturn(pn->pn_kid1);
        if (pn->pn_kid2) {
            JS_ASSERT(pn->pn_kid2->pn_arity == PN_LIST);
            for (pn2 = pn->pn_kid2->pn_head; pn2; pn2 = pn2->pn_next)
                rv &= HasFinalReturn(pn2);
        }
        return rv;

      case TOK_CATCH:
        /* Check this catch block's body. */
        return HasFinalReturn(pn->pn_kid3);

      case TOK_LET:
        /* Non-binary let statements are let declarations. */
        if (pn->pn_arity != PN_BINARY)
            return ENDS_IN_OTHER;
        return HasFinalReturn(pn->pn_right);

      default:
        return ENDS_IN_OTHER;
    }
}

static JSBool
ReportBadReturn(JSContext *cx, JSTreeContext *tc, uintN flags, uintN errnum,
                uintN anonerrnum)
{
    const char *name;

    JS_ASSERT(tc->flags & TCF_IN_FUNCTION);
    if (tc->fun->atom) {
        name = js_AtomToPrintableString(cx, tc->fun->atom);
    } else {
        errnum = anonerrnum;
        name = NULL;
    }
    return js_ReportCompileErrorNumber(cx, TS(tc->compiler), NULL, flags,
                                       errnum, name);
}

static JSBool
CheckFinalReturn(JSContext *cx, JSTreeContext *tc, JSParseNode *pn)
{
    JS_ASSERT(tc->flags & TCF_IN_FUNCTION);
    return HasFinalReturn(pn) == ENDS_IN_RETURN ||
           ReportBadReturn(cx, tc, JSREPORT_WARNING | JSREPORT_STRICT,
                           JSMSG_NO_RETURN_VALUE, JSMSG_ANON_NO_RETURN_VALUE);
}

static JSParseNode *
FunctionBody(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSStmtInfo stmtInfo;
    uintN oldflags, firstLine;
    JSParseNode *pn;

    JS_ASSERT(tc->flags & TCF_IN_FUNCTION);
    js_PushStatement(tc, &stmtInfo, STMT_BLOCK, -1);
    stmtInfo.flags = SIF_BODY_BLOCK;

    oldflags = tc->flags;
    tc->flags &= ~(TCF_RETURN_EXPR | TCF_RETURN_VOID);

    /*
     * Save the body's first line, and store it in pn->pn_pos.begin.lineno
     * later, because we may have not peeked in ts yet, so Statements won't
     * acquire a valid pn->pn_pos.begin from the current token.
     */
    firstLine = ts->lineno;
#if JS_HAS_EXPR_CLOSURES
    if (CURRENT_TOKEN(ts).type == TOK_LC) {
        pn = Statements(cx, ts, tc);
    } else {
        pn = NewParseNode(PN_UNARY, tc);
        if (pn) {
            pn->pn_kid = AssignExpr(cx, ts, tc);
            if (!pn->pn_kid) {
                pn = NULL;
            } else {
                if (tc->flags & TCF_FUN_IS_GENERATOR) {
                    ReportBadReturn(cx, tc, JSREPORT_ERROR,
                                    JSMSG_BAD_GENERATOR_RETURN,
                                    JSMSG_BAD_ANON_GENERATOR_RETURN);
                    pn = NULL;
                } else {
                    pn->pn_type = TOK_RETURN;
                    pn->pn_op = JSOP_RETURN;
                    pn->pn_pos.end = pn->pn_kid->pn_pos.end;
                }
            }
        }
    }
#else
    pn = Statements(cx, ts, tc);
#endif

    if (pn) {
        JS_ASSERT(!(tc->topStmt->flags & SIF_SCOPE));
        js_PopStatement(tc);
        pn->pn_pos.begin.lineno = firstLine;

        /* Check for falling off the end of a function that returns a value. */
        if (JS_HAS_STRICT_OPTION(cx) && (tc->flags & TCF_RETURN_EXPR) &&
            !CheckFinalReturn(cx, tc, pn)) {
            pn = NULL;
        }
    }

    tc->flags = oldflags | (tc->flags & TCF_FUN_FLAGS);
    return pn;
}

static JSAtomListElement *
MakePlaceholder(JSParseNode *pn, JSTreeContext *tc)
{
    JSAtomListElement *ale = tc->lexdeps.add(tc->compiler, pn->pn_atom);
    if (!ale)
        return NULL;

    JSDefinition *dn = (JSDefinition *)
        NewNameNode(tc->compiler->context, pn->pn_atom, tc);
    if (!dn)
        return NULL;

    ALE_SET_DEFN(ale, dn);
    dn->pn_defn = true;
    dn->pn_dflags |= PND_PLACEHOLDER;
    return ale;
}

static bool
Define(JSParseNode *pn, JSAtom *atom, JSTreeContext *tc, bool let = false)
{
    JS_ASSERT(!pn->pn_used);
    JS_ASSERT_IF(pn->pn_defn, pn->isPlaceholder());

    JSHashEntry **hep;
    JSAtomListElement *ale = NULL;
    JSAtomList *list = NULL;

    if (let)
        ale = (list = &tc->decls)->rawLookup(atom, hep);
    if (!ale)
        ale = (list = &tc->lexdeps)->rawLookup(atom, hep);

    if (ale) {
        JSDefinition *dn = ALE_DEFN(ale);
        if (dn != pn) {
            JSParseNode **pnup = &dn->dn_uses;
            JSParseNode *pnu;
            uintN start = let ? pn->pn_blockid : tc->bodyid;

            while ((pnu = *pnup) != NULL && pnu->pn_blockid >= start) {
                JS_ASSERT(pnu->pn_used);
                pnu->pn_lexdef = (JSDefinition *) pn;
                pn->pn_dflags |= pnu->pn_dflags & PND_USE2DEF_FLAGS;
                pnup = &pnu->pn_link;
            }

            if (pnu != dn->dn_uses) {
                *pnup = pn->dn_uses;
                pn->dn_uses = dn->dn_uses;
                dn->dn_uses = pnu;

                if ((!pnu || pnu->pn_blockid < tc->bodyid) && list != &tc->decls)
                    list->rawRemove(tc->compiler, ale, hep);
            }
        }
    }

    ale = tc->decls.add(tc->compiler, atom, let ? JSAtomList::SHADOW : JSAtomList::UNIQUE);
    if (!ale)
        return false;
    ALE_SET_DEFN(ale, pn);
    pn->pn_defn = true;
    pn->pn_dflags &= ~PND_PLACEHOLDER;
    return true;
}

static void
LinkUseToDef(JSParseNode *pn, JSDefinition *dn, JSTreeContext *tc)
{
    JS_ASSERT(!pn->pn_used);
    JS_ASSERT(!pn->pn_defn);
    JS_ASSERT(pn != dn->dn_uses);
    pn->pn_link = dn->dn_uses;
    dn->dn_uses = pn;
    dn->pn_dflags |= pn->pn_dflags & PND_USE2DEF_FLAGS;
    pn->pn_used = true;
    pn->pn_lexdef = dn;
}

static void
ForgetUse(JSParseNode *pn)
{
    if (!pn->pn_used) {
        JS_ASSERT(!pn->pn_defn);
        return;
    }

    JSParseNode **pnup = &pn->lexdef()->dn_uses;
    JSParseNode *pnu;
    while ((pnu = *pnup) != pn)
        pnup = &pnu->pn_link;
    *pnup = pn->pn_link;
    pn->pn_used = false;
}

static JSParseNode *
MakeAssignment(JSParseNode *pn, JSParseNode *rhs, JSTreeContext *tc)
{
    JSParseNode *lhs = NewOrRecycledNode(tc);
    if (!lhs)
        return NULL;
    *lhs = *pn;

    if (pn->pn_used) {
        JSDefinition *dn = pn->pn_lexdef;
        JSParseNode **pnup = &dn->dn_uses;

        while (*pnup != pn)
            pnup = &(*pnup)->pn_link;
        *pnup = lhs;
        lhs->pn_link = pn->pn_link;
        pn->pn_link = NULL;
    }

    pn->pn_type = TOK_ASSIGN;
    pn->pn_op = JSOP_NOP;
    pn->pn_arity = PN_BINARY;
    pn->pn_parens = false;
    pn->pn_used = pn->pn_defn = false;
    pn->pn_left = lhs;
    pn->pn_right = rhs;
    return lhs;
}

static JSParseNode *
MakeDefIntoUse(JSDefinition *dn, JSParseNode *pn, JSAtom *atom, JSTreeContext *tc)
{
    /*
     * If dn is var, const, or let, and it has an initializer, then we must
     * rewrite it to be an assignment node, whose freshly allocated left-hand
     * side becomes a use of pn.
     */
    if (dn->isBindingForm()) {
        JSParseNode *rhs = dn->expr();
        if (rhs) {
            JSParseNode *lhs = MakeAssignment(dn, rhs, tc);
            if (!lhs)
                return NULL;
            //pn->dn_uses = lhs;
            dn = (JSDefinition *) lhs;
        }

        dn->pn_op = (js_CodeSpec[dn->pn_op].format & JOF_SET) ? JSOP_SETNAME : JSOP_NAME;
    } else if (dn->kind() == JSDefinition::FUNCTION) {
        JS_ASSERT(dn->isTopLevel());
        JS_ASSERT(dn->pn_op == JSOP_NOP);
        dn->pn_type = TOK_NAME;
        dn->pn_arity = PN_NAME;
        dn->pn_atom = atom;
    }

    /* Now make dn no longer a definition, rather a use of pn. */
    JS_ASSERT(dn->pn_type == TOK_NAME);
    JS_ASSERT(dn->pn_arity == PN_NAME);
    JS_ASSERT(dn->pn_atom == atom);

    for (JSParseNode *pnu = dn->dn_uses; pnu; pnu = pnu->pn_link) {
        JS_ASSERT(pnu->pn_used);
        JS_ASSERT(!pnu->pn_defn);
        pnu->pn_lexdef = (JSDefinition *) pn;
        pn->pn_dflags |= pnu->pn_dflags & PND_USE2DEF_FLAGS;
    }
    pn->pn_dflags |= dn->pn_dflags & PND_USE2DEF_FLAGS;
    pn->dn_uses = dn;

    dn->pn_defn = false;
    dn->pn_used = true;
    dn->pn_lexdef = (JSDefinition *) pn;
    dn->pn_cookie = FREE_UPVAR_COOKIE;
    dn->pn_dflags &= ~PND_BOUND;
    return dn;
}

static bool
DefineArg(JSParseNode *pn, JSAtom *atom, uintN i, JSTreeContext *tc)
{
    JSParseNode *argpn, *argsbody;

    /* Flag tc so we don't have to lookup arguments on every use. */
    if (atom == tc->compiler->context->runtime->atomState.argumentsAtom)
        tc->flags |= TCF_FUN_PARAM_ARGUMENTS;

    /*
     * Make an argument definition node, distinguished by being in tc->decls
     * but having TOK_NAME type and JSOP_NOP op. Insert it in a TOK_ARGSBODY
     * list node returned via pn->pn_body.
     */
    argpn = NewNameNode(tc->compiler->context, atom, tc);
    if (!argpn)
        return false;
    JS_ASSERT(PN_TYPE(argpn) == TOK_NAME && PN_OP(argpn) == JSOP_NOP);

    /* Arguments are initialized by definition. */
    argpn->pn_dflags |= PND_INITIALIZED;
    if (!Define(argpn, atom, tc))
        return false;

    argsbody = pn->pn_body;
    if (!argsbody) {
        argsbody = NewParseNode(PN_LIST, tc);
        if (!argsbody)
            return false;
        argsbody->pn_type = TOK_ARGSBODY;
        argsbody->pn_op = JSOP_NOP;
        argsbody->makeEmpty();
        pn->pn_body = argsbody;
    }
    argsbody->append(argpn);

    argpn->pn_op = JSOP_GETARG;
    argpn->pn_cookie = MAKE_UPVAR_COOKIE(tc->staticLevel, i);
    argpn->pn_dflags |= PND_BOUND;
    return true;
}

/*
 * Compile a JS function body, which might appear as the value of an event
 * handler attribute in an HTML <INPUT> tag.
 */
bool
JSCompiler::compileFunctionBody(JSContext *cx, JSFunction *fun, JSPrincipals *principals,
                                const jschar *chars, size_t length,
                                const char *filename, uintN lineno)
{
    JSCompiler jsc(cx, principals);

    if (!jsc.init(chars, length, NULL, filename, lineno))
        return false;

    /* No early return from after here until the js_FinishArenaPool calls. */
    JSArenaPool codePool, notePool;
    JS_INIT_ARENA_POOL(&codePool, "code", 1024, sizeof(jsbytecode),
                       &cx->scriptStackQuota);
    JS_INIT_ARENA_POOL(&notePool, "note", 1024, sizeof(jssrcnote),
                       &cx->scriptStackQuota);

    JSCodeGenerator funcg(&jsc, &codePool, &notePool, jsc.tokenStream.lineno);
    funcg.flags |= TCF_IN_FUNCTION;
    funcg.fun = fun;
    if (!GenerateBlockId(&funcg, funcg.bodyid))
        return NULL;

    /* FIXME: make Function format the source for a function definition. */
    jsc.tokenStream.tokens[0].type = TOK_NAME;
    JSParseNode *fn = NewParseNode(PN_FUNC, &funcg);
    if (fn) {
        fn->pn_body = NULL;
        fn->pn_cookie = FREE_UPVAR_COOKIE;

        uintN nargs = fun->nargs;
        if (nargs) {
            jsuword *names = js_GetLocalNameArray(cx, fun, &cx->tempPool);
            if (!names) {
                fn = NULL;
            } else {
                for (uintN i = 0; i < nargs; i++) {
                    JSAtom *name = JS_LOCAL_NAME_TO_ATOM(names[i]);
                    if (!DefineArg(fn, name, i, &funcg)) {
                        fn = NULL;
                        break;
                    }
                }
            }
        }
    }

    /*
     * Farble the body so that it looks like a block statement to js_EmitTree,
     * which is called from js_EmitFunctionBody (see jsemit.cpp).  After we're
     * done parsing, we must fold constants, analyze any nested functions, and
     * generate code for this function, including a stop opcode at the end.
     */
    CURRENT_TOKEN(&jsc.tokenStream).type = TOK_LC;
    JSParseNode *pn = fn ? FunctionBody(cx, &jsc.tokenStream, &funcg) : NULL;
    if (pn) {
        if (!js_MatchToken(cx, &jsc.tokenStream, TOK_EOF)) {
            js_ReportCompileErrorNumber(cx, &jsc.tokenStream, NULL,
                                        JSREPORT_ERROR, JSMSG_SYNTAX_ERROR);
            pn = NULL;
        } else if (!js_FoldConstants(cx, pn, &funcg)) {
            /* js_FoldConstants reported the error already. */
            pn = NULL;
        } else if (funcg.functionList &&
                   !jsc.analyzeFunctions(funcg.functionList, funcg.flags)) {
            pn = NULL;
        } else {
            if (fn->pn_body) {
                JS_ASSERT(PN_TYPE(fn->pn_body) == TOK_ARGSBODY);
                fn->pn_body->append(pn);
                fn->pn_body->pn_pos = pn->pn_pos;
                pn = fn->pn_body;
            }

            if (!js_EmitFunctionScript(cx, &funcg, pn))
                pn = NULL;
        }
    }

    /* Restore saved state and release code generation arenas. */
    JS_FinishArenaPool(&codePool);
    JS_FinishArenaPool(&notePool);
    return pn != NULL;
}

/*
 * Parameter block types for the several Binder functions.  We use a common
 * helper function signature in order to share code among destructuring and
 * simple variable declaration parsers.  In the destructuring case, the binder
 * function is called indirectly from the variable declaration parser by way
 * of CheckDestructuring and its friends.
 */
typedef struct BindData BindData;

typedef JSBool
(*Binder)(JSContext *cx, BindData *data, JSAtom *atom, JSTreeContext *tc);

struct BindData {
    BindData() : fresh(true) {}

    JSParseNode     *pn;        /* name node for definition processing and
                                   error source coordinates */
    JSOp            op;         /* prolog bytecode or nop */
    Binder          binder;     /* binder, discriminates u */
    union {
        struct {
            uintN   overflow;
        } let;
    };
    bool fresh;
};

static JSBool
BindLocalVariable(JSContext *cx, JSFunction *fun, JSAtom *atom,
                  JSLocalKind localKind)
{
    JS_ASSERT(localKind == JSLOCAL_VAR || localKind == JSLOCAL_CONST);

    /*
     * Don't bind a variable with the hidden name 'arguments', per ECMA-262.
     * Instead 'var arguments' always restates the predefined property of the
     * activation objects whose name is 'arguments'. Assignment to such a
     * variable must be handled specially.
     */
    if (atom == cx->runtime->atomState.argumentsAtom)
        return JS_TRUE;

    return js_AddLocal(cx, fun, atom, localKind);
}

#if JS_HAS_DESTRUCTURING
/*
 * Forward declaration to maintain top-down presentation.
 */
static JSParseNode *
DestructuringExpr(JSContext *cx, BindData *data, JSTreeContext *tc,
                  JSTokenType tt);

static JSBool
BindDestructuringArg(JSContext *cx, BindData *data, JSAtom *atom,
                     JSTreeContext *tc)
{
    JSAtomListElement *ale;
    JSParseNode *pn;

    /* Flag tc so we don't have to lookup arguments on every use. */
    if (atom == tc->compiler->context->runtime->atomState.argumentsAtom)
        tc->flags |= TCF_FUN_PARAM_ARGUMENTS;

    JS_ASSERT(tc->flags & TCF_IN_FUNCTION);
    ale = tc->decls.lookup(atom);
    pn = data->pn;
    if (!ale && !Define(pn, atom, tc))
        return JS_FALSE;

    JSLocalKind localKind = js_LookupLocal(cx, tc->fun, atom, NULL);
    if (localKind != JSLOCAL_NONE) {
        js_ReportCompileErrorNumber(cx, TS(tc->compiler), NULL,
                                    JSREPORT_ERROR, JSMSG_DESTRUCT_DUP_ARG);
        return JS_FALSE;
    }

    uintN index = tc->fun->u.i.nvars;
    if (!BindLocalVariable(cx, tc->fun, atom, JSLOCAL_VAR))
        return JS_FALSE;
    pn->pn_op = JSOP_SETLOCAL;
    pn->pn_cookie = MAKE_UPVAR_COOKIE(tc->staticLevel, index);
    pn->pn_dflags |= PND_BOUND;
    return JS_TRUE;
}
#endif /* JS_HAS_DESTRUCTURING */

JSFunction *
JSCompiler::newFunction(JSTreeContext *tc, JSAtom *atom, uintN lambda)
{
    JSObject *parent;
    JSFunction *fun;

    JS_ASSERT((lambda & ~JSFUN_LAMBDA) == 0);

    /*
     * Find the global compilation context in order to pre-set the newborn
     * function's parent slot to tc->scopeChain. If the global context is a
     * compile-and-go one, we leave the pre-set parent intact; otherwise we
     * clear parent and proto.
     */
    while (tc->parent)
        tc = tc->parent;
    parent = (tc->flags & TCF_IN_FUNCTION) ? NULL : tc->scopeChain;

    fun = js_NewFunction(context, NULL, NULL, 0, JSFUN_INTERPRETED | lambda,
                         parent, atom);

    if (fun && !(tc->flags & TCF_COMPILE_N_GO)) {
        STOBJ_CLEAR_PARENT(FUN_OBJECT(fun));
        STOBJ_CLEAR_PROTO(FUN_OBJECT(fun));
    }
    return fun;
}

static JSBool
MatchOrInsertSemicolon(JSContext *cx, JSTokenStream *ts)
{
    JSTokenType tt;

    ts->flags |= TSF_OPERAND;
    tt = js_PeekTokenSameLine(cx, ts);
    ts->flags &= ~TSF_OPERAND;
    if (tt == TOK_ERROR)
        return JS_FALSE;
    if (tt != TOK_EOF && tt != TOK_EOL && tt != TOK_SEMI && tt != TOK_RC) {
        js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                    JSMSG_SEMI_BEFORE_STMNT);
        return JS_FALSE;
    }
    (void) js_MatchToken(cx, ts, TOK_SEMI);
    return JS_TRUE;
}

bool
JSCompiler::analyzeFunctions(JSFunctionBox *funbox, uint32& tcflags)
{
    if (!markFunArgs(funbox, tcflags))
        return false;
    setFunctionKinds(funbox, tcflags);
    return true;
}

/*
 * Mark as funargs any functions that reach up to one or more upvars across an
 * already-known funarg. The parser will flag the o_m lambda as a funarg in:
 *
 *   function f(o, p) {
 *       o.m = function o_m(a) {
 *           function g() { return p; }
 *           function h() { return a; }
 *           return g() + h();
 *       }
 *   }
 *
 * but without this extra marking phase, function g will not be marked as a
 * funarg since it is called from within its parent scope. But g reaches up to
 * f's parameter p, so if o_m escapes f's activation scope, g does too and
 * cannot use JSOP_GETUPVAR to reach p. In contast function h neither escapes
 * nor uses an upvar "above" o_m's level.
 *
 * If function g itself contained lambdas that contained non-lambdas that reach
 * up above its level, then those non-lambdas would have to be marked too. This
 * process is potentially exponential in the number of functions, but generally
 * not so complex. But it can't be done during a single recursive traversal of
 * the funbox tree, so we must use a work queue.
 *
 * Return the minimal "skipmin" for funbox and its siblings. This is the delta
 * between the static level of the bodies of funbox and its peers (which must
 * be funbox->level + 1), and the static level of the nearest upvar among all
 * the upvars contained by funbox and its peers. If there are no upvars, return
 * FREE_STATIC_LEVEL. Thus this function never returns 0.
 */
static uintN
FindFunArgs(JSFunctionBox *funbox, int level, JSFunctionBoxQueue *queue)
{
    uintN allskipmin = FREE_STATIC_LEVEL;

    do {
        JSParseNode *fn = funbox->node;
        JSFunction *fun = (JSFunction *) funbox->object;
        int fnlevel = level;

        /*
         * An eval can leak funbox, functions along its ancestor line, and its
         * immediate kids. Since FindFunArgs uses DFS and the parser propagates
         * TCF_FUN_HEAVYWEIGHT bottom up, funbox's ancestor function nodes have
         * already been marked as funargs by this point. Therefore we have to
         * flag only funbox->node and funbox->kids' nodes here.
         */
        if (funbox->tcflags & TCF_FUN_HEAVYWEIGHT) {
            fn->setFunArg();
            for (JSFunctionBox *kid = funbox->kids; kid; kid = kid->siblings)
                kid->node->setFunArg();
        }

        /*
         * Compute in skipmin the least distance from fun's static level up to
         * an upvar, whether used directly by fun, or indirectly by a function
         * nested in fun.
         */
        uintN skipmin = FREE_STATIC_LEVEL;
        JSParseNode *pn = fn->pn_body;

        if (pn->pn_type == TOK_UPVARS) {
            JSAtomList upvars(pn->pn_names);
            JS_ASSERT(upvars.count != 0);

            JSAtomListIterator iter(&upvars);
            JSAtomListElement *ale;

            while ((ale = iter()) != NULL) {
                JSDefinition *lexdep = ALE_DEFN(ale)->resolve();

                if (!lexdep->isFreeVar()) {
                    uintN upvarLevel = lexdep->frameLevel();

                    if (int(upvarLevel) <= fnlevel)
                        fn->setFunArg();

                    uintN skip = (funbox->level + 1) - upvarLevel;
                    if (skip < skipmin)
                        skipmin = skip;
                }
            }
        }

        /*
         * If this function escapes, whether directly (the parser detects such
         * escapes) or indirectly (because this non-escaping function uses an
         * upvar that reaches across an outer function boundary where the outer
         * function escapes), enqueue it for further analysis, and bump fnlevel
         * to trap any non-escaping children.
         */
        if (fn->isFunArg()) {
            queue->push(funbox);
            fnlevel = int(funbox->level);
        }

        /*
         * Now process the current function's children, and recalibrate their
         * cumulative skipmin to be relative to the current static level.
         */
        if (funbox->kids) {
            uintN kidskipmin = FindFunArgs(funbox->kids, fnlevel, queue);

            JS_ASSERT(kidskipmin != 0);
            if (kidskipmin != FREE_STATIC_LEVEL) {
                --kidskipmin;
                if (kidskipmin != 0 && kidskipmin < skipmin)
                    skipmin = kidskipmin;
            }
        }

        /*
         * Finally, after we've traversed all of the current function's kids,
         * minimize fun's skipmin against our accumulated skipmin. Do likewise
         * with allskipmin, but minimize across funbox and all of its siblings,
         * to compute our return value.
         */
        if (skipmin != FREE_STATIC_LEVEL) {
            fun->u.i.skipmin = skipmin;
            if (skipmin < allskipmin)
                allskipmin = skipmin;
        }
    } while ((funbox = funbox->siblings) != NULL);

    return allskipmin;
}

bool
JSCompiler::markFunArgs(JSFunctionBox *funbox, uintN tcflags)
{
    JSFunctionBoxQueue queue;
    if (!queue.init(functionCount))
        return false;

    FindFunArgs(funbox, -1, &queue);
    while ((funbox = queue.pull()) != NULL) {
        JSParseNode *fn = funbox->node;
        JS_ASSERT(fn->isFunArg());

        JSParseNode *pn = fn->pn_body;
        if (pn->pn_type == TOK_UPVARS) {
            JSAtomList upvars(pn->pn_names);
            JS_ASSERT(upvars.count != 0);

            JSAtomListIterator iter(&upvars);
            JSAtomListElement *ale;

            while ((ale = iter()) != NULL) {
                JSDefinition *lexdep = ALE_DEFN(ale)->resolve();

                if (!lexdep->isFreeVar() &&
                    !lexdep->isFunArg() &&
                    lexdep->kind() == JSDefinition::FUNCTION) {
                    /*
                     * Mark this formerly-Algol-like function as an escaping
                     * function (i.e., as a funarg), because it is used from a
                     * funarg and therefore can not use JSOP_{GET,CALL}UPVAR to
                     * access upvars.
                     *
                     * Progress is guaranteed because we set the funarg flag
                     * here, which suppresses revisiting this function (thanks
                     * to the !lexdep->isFunArg() test just above).
                     */
                    lexdep->setFunArg();

                    JSFunctionBox *afunbox = lexdep->pn_funbox;
                    queue.push(afunbox);

                    /*
                     * Walk over nested functions again, now that we have
                     * changed the level across which it is unsafe to access
                     * upvars using the runtime dynamic link (frame chain).
                     */
                    if (afunbox->kids)
                        FindFunArgs(afunbox->kids, afunbox->level, &queue);
                }
            }
        }
    }
    return true;
}

static uint32
MinBlockId(JSParseNode *fn, uint32 id)
{
    if (fn->pn_blockid < id)
        return false;
    if (fn->pn_defn) {
        for (JSParseNode *pn = fn->dn_uses; pn; pn = pn->pn_link) {
            if (pn->pn_blockid < id)
                return false;
        }
    }
    return true;
}

static bool
OneBlockId(JSParseNode *fn, uint32 id)
{
    if (fn->pn_blockid != id)
        return false;
    if (fn->pn_defn) {
        for (JSParseNode *pn = fn->dn_uses; pn; pn = pn->pn_link) {
            if (pn->pn_blockid != id)
                return false;
        }
    }
    return true;
}

void
JSCompiler::setFunctionKinds(JSFunctionBox *funbox, uint32& tcflags)
{
#ifdef JS_FUNCTION_METERING
# define FUN_METER(x)   JS_RUNTIME_METER(context->runtime, functionMeter.x)
#else
# define FUN_METER(x)   ((void)0)
#endif
    JSFunctionBox *parent = funbox->parent;

    for (;;) {
        JSParseNode *fn = funbox->node;

        if (funbox->kids)
            setFunctionKinds(funbox->kids, tcflags);

        JSParseNode *pn = fn->pn_body;
        JSFunction *fun = (JSFunction *) funbox->object;

        FUN_METER(allfun);
        if (funbox->tcflags & TCF_FUN_HEAVYWEIGHT) {
            FUN_METER(heavy);
            JS_ASSERT(FUN_KIND(fun) == JSFUN_INTERPRETED);
        } else if (pn->pn_type != TOK_UPVARS) {
            /*
             * No lexical dependencies => null closure, for best performance.
             * A null closure needs no scope chain, but alas we've coupled
             * principals-finding to scope (for good fundamental reasons, but
             * the implementation overloads the parent slot and we should fix
             * that). See, e.g., the JSOP_LAMBDA case in jsinterp.cpp.
             *
             * In more detail: the ES3 spec allows the implementation to create
             * "joined function objects", or not, at its discretion. But real-
             * world implementations always create unique function objects for
             * closures, and this can be detected via mutation. Open question:
             * do popular implementations create unique function objects for
             * null closures?
             *
             * FIXME: bug 476950.
             */
            FUN_METER(nofreeupvar);
            FUN_SET_KIND(fun, JSFUN_NULL_CLOSURE);
        } else {
            JSAtomList upvars(pn->pn_names);
            JS_ASSERT(upvars.count != 0);

            JSAtomListIterator iter(&upvars);
            JSAtomListElement *ale;

            if (!fn->isFunArg()) {
                /*
                 * This function is Algol-like, it never escapes. So long as it
                 * does not assign to outer variables, it needs only an upvars
                 * array in its script and JSOP_{GET,CALL}UPVAR opcodes in its
                 * bytecode to reach up the frame stack at runtime based on
                 * those upvars' cookies.
                 *
                 * Any assignments to upvars from functions called by this one
                 * will be coherent because of the JSOP_{GET,CALL}UPVAR ops,
                 * which load from stack homes when interpreting or from native
                 * stack slots when executing a trace.
                 *
                 * We could add JSOP_SETUPVAR, etc., but it is uncommon for a
                 * nested function to assign to an outer lexical variable, so
                 * we defer adding yet more code footprint in the absence of
                 * evidence motivating these opcodes.
                 */
                bool mutation = !!(funbox->tcflags & TCF_FUN_SETS_OUTER_NAME);
                uintN nupvars = 0;

                /*
                 * Check that at least one outer lexical binding was assigned
                 * to (global variables don't count). This is conservative: we
                 * could limit assignments to those in the current function,
                 * but that's too much work. As with flat closures (handled
                 * below), we optimize for the case where outer bindings are
                 * not reassigned anywhere.
                 */
                while ((ale = iter()) != NULL) {
                    JSDefinition *lexdep = ALE_DEFN(ale)->resolve();

                    if (!lexdep->isFreeVar()) {
                        JS_ASSERT(lexdep->frameLevel() <= funbox->level);
                        ++nupvars;
                        if (lexdep->isAssigned())
                            break;
                    }
                }
                if (!ale)
                    mutation = false;

                if (nupvars == 0) {
                    FUN_METER(onlyfreevar);
                    FUN_SET_KIND(fun, JSFUN_NULL_CLOSURE);
                } else if (!mutation && !(funbox->tcflags & TCF_FUN_IS_GENERATOR)) {
                    /*
                     * Algol-like functions can read upvars using the dynamic
                     * link (cx->fp/fp->down). They do not need to entrain and
                     * search their environment.
                     */
                    FUN_METER(display);
                    FUN_SET_KIND(fun, JSFUN_NULL_CLOSURE);
                } else {
                    if (!(funbox->tcflags & TCF_FUN_IS_GENERATOR))
                        FUN_METER(setupvar);
                }
            } else {
                uintN nupvars = 0;

                /*
                 * For each lexical dependency from this closure to an outer
                 * binding, analyze whether it is safe to copy the binding's
                 * value into a flat closure slot when the closure is formed.
                 */
                while ((ale = iter()) != NULL) {
                    JSDefinition *lexdep = ALE_DEFN(ale)->resolve();

                    if (!lexdep->isFreeVar()) {
                        ++nupvars;

                        /*
                         * Consider the current function (the lambda, innermost
                         * below) using a var x defined two static levels up:
                         *
                         *  function f() {
                         *      // z = g();
                         *      var x = 42;
                         *      function g() {
                         *          return function () { return x; };
                         *      }
                         *      return g();
                         *  }
                         *
                         * So long as (1) the initialization in 'var x = 42'
                         * dominates all uses of g and (2) x is not reassigned,
                         * it is safe to optimize the lambda to a flat closure.
                         * Uncommenting the early call to g makes it unsafe to
                         * so optimize (z could name a global setter that calls
                         * its argument).
                         */
                        JSFunctionBox *afunbox = funbox;
                        uintN lexdepLevel = lexdep->frameLevel();

                        JS_ASSERT(lexdepLevel <= funbox->level);
                        while (afunbox->level != lexdepLevel) {
                            afunbox = afunbox->parent;

                            /*
                             * afunbox can't be null because we are sure
                             * to find a function box whose level == lexdepLevel
                             * before walking off the top of the funbox tree.
                             * See bug 493260 comments 16-18.
                             *
                             * Assert but check anyway, to check future changes
                             * that bind eval upvars in the parser.
                             */
                            JS_ASSERT(afunbox);

                            /*
                             * If this function is reaching up across an
                             * enclosing funarg, we cannot make a flat
                             * closure. The display stops working once the
                             * funarg escapes.
                             */
                            if (!afunbox || afunbox->node->isFunArg())
                                goto break2;
                        }

                        /*
                         * If afunbox's function (which is at the same level as
                         * lexdep) is in a loop, pessimistically assume the
                         * variable initializer may be in the same loop. A flat
                         * closure would then be unsafe, as the captured
                         * variable could be assigned after the closure is
                         * created. See bug 493232.
                         */
                        if (afunbox->inLoop)
                            break;

                        /*
                         * with and eval defeat lexical scoping; eval anywhere
                         * in a variable's scope can assign to it. Both defeat
                         * the flat closure optimization. The parser detects
                         * these cases and flags the function heavyweight.
                         */
                        if ((afunbox->parent ? afunbox->parent->tcflags : tcflags)
                            & TCF_FUN_HEAVYWEIGHT) {
                            break;
                        }

                        /*
                         * If afunbox's function is not a lambda, it will be
                         * hoisted, so it could capture the undefined value
                         * that by default initializes var/let/const
                         * bindings. And if lexdep is a function that comes at
                         * (meaning a function refers to its own name) or
                         * strictly after afunbox, we also break to defeat the
                         * flat closure optimization.
                         */
                        JSFunction *afun = (JSFunction *) afunbox->object;
                        if (!(afun->flags & JSFUN_LAMBDA)) {
                            if (lexdep->isBindingForm())
                                break;
                            if (lexdep->pn_pos >= afunbox->node->pn_pos)
                                break;
                        }

                        if (!lexdep->isInitialized())
                            break;

                        JSDefinition::Kind lexdepKind = lexdep->kind();
                        if (lexdepKind != JSDefinition::CONST) {
                            if (lexdep->isAssigned())
                                break;

                            /*
                             * Any formal could be mutated behind our back via
                             * the arguments object, so deoptimize if the outer
                             * function uses arguments.
                             *
                             * In a Function constructor call where the final
                             * argument -- the body source for the function to
                             * create -- contains a nested function definition
                             * or expression, afunbox->parent will be null. The
                             * body source might use |arguments| outside of any
                             * nested functions it may contain, so we have to
                             * check the tcflags parameter that was passed in
                             * from JSCompiler::compileFunctionBody.
                             */
                            if (lexdepKind == JSDefinition::ARG &&
                                ((afunbox->parent ? afunbox->parent->tcflags : tcflags) &
                                 TCF_FUN_USES_ARGUMENTS)) {
                                break;
                            }
                        }

                        /*
                         * Check quick-and-dirty dominance relation. Function
                         * definitions dominate their uses thanks to hoisting.
                         * Other binding forms hoist as undefined, of course,
                         * so check forward-reference and blockid relations.
                         */
                        if (lexdepKind != JSDefinition::FUNCTION) {
                            /*
                             * Watch out for code such as
                             *
                             *   (function () {
                             *   ...
                             *   var jQuery = ... = function (...) {
                             *       return new jQuery.foo.bar(baz);
                             *   }
                             *   ...
                             *   })();
                             *
                             * where the jQuery var is not reassigned, but of
                             * course is not initialized at the time that the
                             * would-be-flat closure containing the jQuery
                             * upvar is formed.
                             */
                            if (lexdep->pn_pos.end >= afunbox->node->pn_pos.end)
                                break;

                            if (lexdep->isTopLevel()
                                ? !MinBlockId(afunbox->node, lexdep->pn_blockid)
                                : !lexdep->isBlockChild() ||
                                  !afunbox->node->isBlockChild() ||
                                  !OneBlockId(afunbox->node, lexdep->pn_blockid)) {
                                break;
                            }
                        }
                    }
                }

              break2:
                if (nupvars == 0) {
                    FUN_METER(onlyfreevar);
                    FUN_SET_KIND(fun, JSFUN_NULL_CLOSURE);
                } else if (!ale) {
                    /*
                     * We made it all the way through the upvar loop, so it's
                     * safe to optimize to a flat closure.
                     */
                    FUN_METER(flat);
                    FUN_SET_KIND(fun, JSFUN_FLAT_CLOSURE);
                    switch (PN_OP(fn)) {
                      case JSOP_DEFFUN:
                        fn->pn_op = JSOP_DEFFUN_FC;
                        break;
                      case JSOP_DEFLOCALFUN:
                        fn->pn_op = JSOP_DEFLOCALFUN_FC;
                        break;
                      case JSOP_LAMBDA:
                        fn->pn_op = JSOP_LAMBDA_FC;
                        break;
                      default:
                        /* js_EmitTree's case TOK_FUNCTION: will select op. */
                        JS_ASSERT(PN_OP(fn) == JSOP_NOP);
                    }
                } else {
                    FUN_METER(badfunarg);
                }
            }
        }

        if (FUN_KIND(fun) == JSFUN_INTERPRETED) {
            if (pn->pn_type != TOK_UPVARS) {
                if (parent)
                    parent->tcflags |= TCF_FUN_HEAVYWEIGHT;
            } else {
                JSAtomList upvars(pn->pn_names);
                JS_ASSERT(upvars.count != 0);

                JSAtomListIterator iter(&upvars);
                JSAtomListElement *ale;

                /*
                 * One or more upvars cannot be safely snapshot into a flat
                 * closure's dslot (see JSOP_GETDSLOT), so we loop again over
                 * all upvars, and for each non-free upvar, ensure that its
                 * containing function has been flagged as heavyweight.
                 *
                 * The emitter must see TCF_FUN_HEAVYWEIGHT accurately before
                 * generating any code for a tree of nested functions.
                 */
                while ((ale = iter()) != NULL) {
                    JSDefinition *lexdep = ALE_DEFN(ale)->resolve();

                    if (!lexdep->isFreeVar()) {
                        JSFunctionBox *afunbox = funbox->parent;
                        uintN lexdepLevel = lexdep->frameLevel();

                        while (afunbox) {
                            /*
                             * NB: afunbox->level is the static level of
                             * the definition or expression of the function
                             * parsed into afunbox, not the static level of
                             * its body. Therefore we must add 1 to match
                             * lexdep's level to find the afunbox whose
                             * body contains the lexdep definition.
                             */
                            if (afunbox->level + 1U == lexdepLevel ||
                                (lexdepLevel == 0 && lexdep->isLet())) {
                                afunbox->tcflags |= TCF_FUN_HEAVYWEIGHT;
                                break;
                            }
                            afunbox = afunbox->parent;
                        }
                        if (!afunbox && (tcflags & TCF_IN_FUNCTION))
                            tcflags |= TCF_FUN_HEAVYWEIGHT;
                    }
                }
            }
        }

        funbox = funbox->siblings;
        if (!funbox)
            break;
        JS_ASSERT(funbox->parent == parent);
    }
#undef FUN_METER
}

const char js_argument_str[] = "argument";
const char js_variable_str[] = "variable";
const char js_unknown_str[]  = "unknown";

const char *
JSDefinition::kindString(Kind kind)
{
    static const char *table[] = {
        js_var_str, js_const_str, js_let_str,
        js_function_str, js_argument_str, js_unknown_str
    };

    JS_ASSERT(unsigned(kind) <= unsigned(ARG));
    return table[kind];
}

static JSFunctionBox *
EnterFunction(JSParseNode *fn, JSTreeContext *tc, JSTreeContext *funtc,
              JSAtom *funAtom = NULL, uintN lambda = JSFUN_LAMBDA)
{
    JSFunction *fun = tc->compiler->newFunction(tc, funAtom, lambda);
    if (!fun)
        return NULL;

    /* Create box for fun->object early to protect against last-ditch GC. */
    JSFunctionBox *funbox = tc->compiler->newFunctionBox(FUN_OBJECT(fun), fn, tc);
    if (!funbox)
        return NULL;

    /* Initialize non-default members of funtc. */
    funtc->flags |= funbox->tcflags;
    funtc->blockidGen = tc->blockidGen;
    if (!GenerateBlockId(funtc, funtc->bodyid))
        return NULL;
    funtc->fun = fun;
    funtc->funbox = funbox;
    funtc->parent = tc;
    if (!SetStaticLevel(funtc, tc->staticLevel + 1))
        return NULL;

    return funbox;
}

static bool
LeaveFunction(JSParseNode *fn, JSTreeContext *funtc, JSTreeContext *tc,
              JSAtom *funAtom = NULL, uintN lambda = JSFUN_LAMBDA)
{
    tc->blockidGen = funtc->blockidGen;

    fn->pn_funbox->tcflags |= funtc->flags & (TCF_FUN_FLAGS | TCF_COMPILE_N_GO);

    fn->pn_dflags |= PND_INITIALIZED;
    JS_ASSERT_IF(tc->atTopLevel() && lambda == 0 && funAtom,
                 fn->pn_dflags & PND_TOPLEVEL);
    if (!tc->topStmt || tc->topStmt->type == STMT_BLOCK)
        fn->pn_dflags |= PND_BLOCKCHILD;

    /*
     * Propagate unresolved lexical names up to tc->lexdeps, and save a copy
     * of funtc->lexdeps in a TOK_UPVARS node wrapping the function's formal
     * params and body. We do this only if there are lexical dependencies not
     * satisfied by the function's declarations, to avoid penalizing functions
     * that use only their arguments and other local bindings.
     */
    if (funtc->lexdeps.count != 0) {
        JSAtomListIterator iter(&funtc->lexdeps);
        JSAtomListElement *ale;
        int foundCallee = 0;

        while ((ale = iter()) != NULL) {
            JSAtom *atom = ALE_ATOM(ale);
            JSDefinition *dn = ALE_DEFN(ale);
            JS_ASSERT(dn->isPlaceholder());

            if (atom == funAtom && lambda != 0) {
                dn->pn_op = JSOP_CALLEE;
                dn->pn_cookie = MAKE_UPVAR_COOKIE(funtc->staticLevel, CALLEE_UPVAR_SLOT);
                dn->pn_dflags |= PND_BOUND;

                /*
                 * If this named function expression uses its own name other
                 * than to call itself, flag this function specially.
                 */
                if (dn->isFunArg())
                    fn->pn_funbox->tcflags |= TCF_FUN_USES_OWN_NAME;
                foundCallee = 1;
                continue;
            }

            if (!(fn->pn_funbox->tcflags & TCF_FUN_SETS_OUTER_NAME) &&
                dn->isAssigned()) {
                /*
                 * Make sure we do not fail to set TCF_FUN_SETS_OUTER_NAME if
                 * any use of dn in funtc assigns. See NoteLValue for the easy
                 * backward-reference case; this is the hard forward-reference
                 * case where we pay a higher price.
                 */
                for (JSParseNode *pnu = dn->dn_uses; pnu; pnu = pnu->pn_link) {
                    if (pnu->isAssigned() && pnu->pn_blockid >= funtc->bodyid) {
                        fn->pn_funbox->tcflags |= TCF_FUN_SETS_OUTER_NAME;
                        break;
                    }
                }
            }

            JSAtomListElement *outer_ale = tc->decls.lookup(atom);
            if (!outer_ale)
                outer_ale = tc->lexdeps.lookup(atom);
            if (outer_ale) {
                /*
                 * Insert dn's uses list at the front of outer_dn's list.
                 *
                 * Without loss of generality or correctness, we allow a dn to
                 * be in inner and outer lexdeps, since the purpose of lexdeps
                 * is one-pass coordination of name use and definition across
                 * functions, and if different dn's are used we'll merge lists
                 * when leaving the inner function.
                 *
                 * The dn == outer_dn case arises with generator expressions
                 * (see CompExprTransplanter::transplant, the PN_FUNC/PN_NAME
                 * case), and nowhere else, currently.
                 */
                JSDefinition *outer_dn = ALE_DEFN(outer_ale);

                if (dn != outer_dn) {
                    JSParseNode **pnup = &dn->dn_uses;
                    JSParseNode *pnu;

                    while ((pnu = *pnup) != NULL) {
                        pnu->pn_lexdef = outer_dn;
                        pnup = &pnu->pn_link;
                    }

                    /*
                     * Make dn be a use that redirects to outer_dn, because we
                     * can't replace dn with outer_dn in all the pn_namesets in
                     * the AST where it may be. Instead we make it forward to
                     * outer_dn. See JSDefinition::resolve.
                     */
                    *pnup = outer_dn->dn_uses;
                    outer_dn->dn_uses = dn;
                    outer_dn->pn_dflags |= dn->pn_dflags & ~PND_PLACEHOLDER;
                    dn->pn_defn = false;
                    dn->pn_used = true;
                    dn->pn_lexdef = outer_dn;
                }
            } else {
                /* Add an outer lexical dependency for ale's definition. */
                outer_ale = tc->lexdeps.add(tc->compiler, atom);
                if (!outer_ale)
                    return false;
                ALE_SET_DEFN(outer_ale, ALE_DEFN(ale));
            }
        }

        if (funtc->lexdeps.count - foundCallee != 0) {
            JSParseNode *body = fn->pn_body;

            fn->pn_body = NewParseNode(PN_NAMESET, tc);
            if (!fn->pn_body)
                return false;

            fn->pn_body->pn_type = TOK_UPVARS;
            fn->pn_body->pn_pos = body->pn_pos;
            if (foundCallee)
                funtc->lexdeps.remove(tc->compiler, funAtom);
            fn->pn_body->pn_names = funtc->lexdeps;
            fn->pn_body->pn_tree = body;
        }

        funtc->lexdeps.clear();
    }

    return true;
}

static JSParseNode *
FunctionDef(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc,
            uintN lambda)
{
    JSOp op;
    JSParseNode *pn, *body, *result;
    JSTokenType tt;
    JSAtom *funAtom;
    JSAtomListElement *ale;
#if JS_HAS_DESTRUCTURING
    JSParseNode *item, *list = NULL;
    bool destructuringArg = false, duplicatedArg = false;
#endif

    /* Make a TOK_FUNCTION node. */
#if JS_HAS_GETTER_SETTER
    op = CURRENT_TOKEN(ts).t_op;
#endif
    pn = NewParseNode(PN_FUNC, tc);
    if (!pn)
        return NULL;
    pn->pn_body = NULL;
    pn->pn_cookie = FREE_UPVAR_COOKIE;

    /*
     * If a lambda, give up on JSOP_{GET,CALL}UPVAR usage unless this function
     * is immediately applied (we clear PND_FUNARG if so -- see MemberExpr).
     *
     * Also treat function sub-statements (non-lambda, non-top-level functions)
     * as escaping funargs, since we can't statically analyze their definitions
     * and uses.
     */
    bool topLevel = tc->atTopLevel();
    pn->pn_dflags = (lambda || !topLevel) ? PND_FUNARG : 0;

    /* Scan the optional function name into funAtom. */
    ts->flags |= TSF_KEYWORD_IS_NAME;
    tt = js_GetToken(cx, ts);
    ts->flags &= ~TSF_KEYWORD_IS_NAME;
    if (tt == TOK_NAME) {
        funAtom = CURRENT_TOKEN(ts).t_atom;
    } else {
        if (lambda == 0 && (cx->options & JSOPTION_ANONFUNFIX)) {
            js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                        JSMSG_SYNTAX_ERROR);
            return NULL;
        }
        funAtom = NULL;
        js_UngetToken(ts);
    }

    /*
     * Record names for function statements in tc->decls so we know when to
     * avoid optimizing variable references that might name a function.
     */
    if (lambda == 0 && funAtom) {
        ale = tc->decls.lookup(funAtom);
        if (ale) {
            JSDefinition *dn = ALE_DEFN(ale);
            JSDefinition::Kind dn_kind = dn->kind();

            JS_ASSERT(!dn->pn_used);
            JS_ASSERT(dn->pn_defn);

            if (JS_HAS_STRICT_OPTION(cx) || dn_kind == JSDefinition::CONST) {
                const char *name = js_AtomToPrintableString(cx, funAtom);
                if (!name ||
                    !js_ReportCompileErrorNumber(cx, ts, NULL,
                                                 (dn_kind != JSDefinition::CONST)
                                                 ? JSREPORT_WARNING | JSREPORT_STRICT
                                                 : JSREPORT_ERROR,
                                                 JSMSG_REDECLARED_VAR,
                                                 JSDefinition::kindString(dn_kind),
                                                 name)) {
                    return NULL;
                }
            }

            if (topLevel) {
                ALE_SET_DEFN(ale, pn);
                pn->pn_defn = true;
                pn->dn_uses = dn;               /* dn->dn_uses is now pn_link */

                if (!MakeDefIntoUse(dn, pn, funAtom, tc))
                    return NULL;
            }
        } else if (topLevel) {
            /*
             * If this function was used before it was defined, claim the
             * pre-created definition node for this function that PrimaryExpr
             * put in tc->lexdeps on first forward reference, and recycle pn.
             */
            JSHashEntry **hep;

            ale = tc->lexdeps.rawLookup(funAtom, hep);
            if (ale) {
                JSDefinition *fn = ALE_DEFN(ale);

                JS_ASSERT(fn->pn_defn);
                fn->pn_type = TOK_FUNCTION;
                fn->pn_arity = PN_FUNC;
                fn->pn_pos.begin = pn->pn_pos.begin;
                fn->pn_body = NULL;
                fn->pn_cookie = FREE_UPVAR_COOKIE;

                tc->lexdeps.rawRemove(tc->compiler, ale, hep);
                RecycleTree(pn, tc);
                pn = fn;
            }

            if (!Define(pn, funAtom, tc))
                return NULL;
        }

        /*
         * A function nested at top level inside another's body needs only a
         * local variable to bind its name to its value, and not an activation
         * object property (it might also need the activation property, if the
         * outer function contains with statements, e.g., but the stack slot
         * wins when jsemit.c's BindNameToSlot can optimize a JSOP_NAME into a
         * JSOP_GETLOCAL bytecode).
         */
        if (topLevel) {
            pn->pn_dflags |= PND_TOPLEVEL;

            if (tc->flags & TCF_IN_FUNCTION) {
                JSLocalKind localKind;
                uintN index;

                /*
                 * Define a local in the outer function so that BindNameToSlot
                 * can properly optimize accesses. Note that we need a local
                 * variable, not an argument, for the function statement. Thus
                 * we add a variable even if a parameter with the given name
                 * already exists.
                 */
                localKind = js_LookupLocal(cx, tc->fun, funAtom, &index);
                switch (localKind) {
                  case JSLOCAL_NONE:
                  case JSLOCAL_ARG:
                    index = tc->fun->u.i.nvars;
                    if (!js_AddLocal(cx, tc->fun, funAtom, JSLOCAL_VAR))
                        return NULL;
                    /* FALL THROUGH */

                  case JSLOCAL_VAR:
                    pn->pn_cookie = MAKE_UPVAR_COOKIE(tc->staticLevel, index);
                    pn->pn_dflags |= PND_BOUND;
                    break;

                  default:;
                }
            }
        }
    }

    /* Initialize early for possible flags mutation via DestructuringExpr. */
    JSTreeContext funtc(tc->compiler);

    JSFunctionBox *funbox = EnterFunction(pn, tc, &funtc, funAtom, lambda);
    if (!funbox)
        return NULL;

    JSFunction *fun = (JSFunction *) funbox->object;

#if JS_HAS_GETTER_SETTER
    if (op != JSOP_NOP)
        fun->flags |= (op == JSOP_GETTER) ? JSPROP_GETTER : JSPROP_SETTER;
#endif

    /* Now parse formal argument list and compute fun->nargs. */
    MUST_MATCH_TOKEN(TOK_LP, JSMSG_PAREN_BEFORE_FORMAL);
    if (!js_MatchToken(cx, ts, TOK_RP)) {
        do {
            tt = js_GetToken(cx, ts);
            switch (tt) {
#if JS_HAS_DESTRUCTURING
              case TOK_LB:
              case TOK_LC:
              {
                BindData data;
                JSParseNode *lhs, *rhs;
                jsint slot;

                /* See comment below in the TOK_NAME case. */
                if (duplicatedArg)
                    goto report_dup_and_destructuring;
                destructuringArg = true;

                /*
                 * A destructuring formal parameter turns into one or more
                 * local variables initialized from properties of a single
                 * anonymous positional parameter, so here we must tweak our
                 * binder and its data.
                 */
                data.pn = NULL;
                data.op = JSOP_DEFVAR;
                data.binder = BindDestructuringArg;
                lhs = DestructuringExpr(cx, &data, &funtc, tt);
                if (!lhs)
                    return NULL;

                /*
                 * Adjust fun->nargs to count the single anonymous positional
                 * parameter that is to be destructured.
                 */
                slot = fun->nargs;
                if (!js_AddLocal(cx, fun, NULL, JSLOCAL_ARG))
                    return NULL;

                /*
                 * Synthesize a destructuring assignment from the single
                 * anonymous positional parameter into the destructuring
                 * left-hand-side expression and accumulate it in list.
                 */
                rhs = NewNameNode(cx, cx->runtime->atomState.emptyAtom, &funtc);
                if (!rhs)
                    return NULL;
                rhs->pn_type = TOK_NAME;
                rhs->pn_op = JSOP_GETARG;
                rhs->pn_cookie = MAKE_UPVAR_COOKIE(funtc.staticLevel, slot);
                rhs->pn_dflags |= PND_BOUND;

                item = NewBinary(TOK_ASSIGN, JSOP_NOP, lhs, rhs, &funtc);
                if (!item)
                    return NULL;
                if (!list) {
                    list = NewParseNode(PN_LIST, &funtc);
                    if (!list)
                        return NULL;
                    list->pn_type = TOK_COMMA;
                    list->makeEmpty();
                }
                list->append(item);
                break;
              }
#endif /* JS_HAS_DESTRUCTURING */

              case TOK_NAME:
              {
                /*
                 * Check for a duplicate parameter name, a "feature" that
                 * ECMA-262 requires. This is a SpiderMonkey strict warning,
                 * soon to be an ES3.1 strict error.
                 *
                 * Further, if any argument is a destructuring pattern, forbid
                 * duplicates. We will report the error either now if we have
                 * seen a destructuring pattern already, or later when we find
                 * the first pattern.
                 */
                JSAtom *atom = CURRENT_TOKEN(ts).t_atom;
                if (JS_HAS_STRICT_OPTION(cx) &&
                    js_LookupLocal(cx, fun, atom, NULL) != JSLOCAL_NONE) {
#if JS_HAS_DESTRUCTURING
                    if (destructuringArg)
                        goto report_dup_and_destructuring;
                    duplicatedArg = true;
#endif
                    const char *name = js_AtomToPrintableString(cx, atom);
                    if (!name ||
                        !js_ReportCompileErrorNumber(cx, TS(funtc.compiler),
                                                     NULL,
                                                     JSREPORT_WARNING |
                                                     JSREPORT_STRICT,
                                                     JSMSG_DUPLICATE_FORMAL,
                                                     name)) {
                        return NULL;
                    }
                }
                if (!DefineArg(pn, atom, fun->nargs, &funtc))
                    return NULL;
                if (!js_AddLocal(cx, fun, atom, JSLOCAL_ARG))
                    return NULL;
                break;
              }

              default:
                js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                            JSMSG_MISSING_FORMAL);
                /* FALL THROUGH */
              case TOK_ERROR:
                return NULL;

#if JS_HAS_DESTRUCTURING
              report_dup_and_destructuring:
                js_ReportCompileErrorNumber(cx, TS(tc->compiler), NULL,
                                            JSREPORT_ERROR,
                                            JSMSG_DESTRUCT_DUP_ARG);
                return NULL;
#endif
            }
        } while (js_MatchToken(cx, ts, TOK_COMMA));

        MUST_MATCH_TOKEN(TOK_RP, JSMSG_PAREN_AFTER_FORMAL);
    }

#if JS_HAS_EXPR_CLOSURES
    ts->flags |= TSF_OPERAND;
    tt = js_GetToken(cx, ts);
    ts->flags &= ~TSF_OPERAND;
    if (tt != TOK_LC) {
        js_UngetToken(ts);
        fun->flags |= JSFUN_EXPR_CLOSURE;
    }
#else
    MUST_MATCH_TOKEN(TOK_LC, JSMSG_CURLY_BEFORE_BODY);
#endif

    body = FunctionBody(cx, ts, &funtc);
    if (!body)
        return NULL;

#if JS_HAS_EXPR_CLOSURES
    if (tt == TOK_LC)
        MUST_MATCH_TOKEN(TOK_RC, JSMSG_CURLY_AFTER_BODY);
    else if (lambda == 0 && !MatchOrInsertSemicolon(cx, ts))
        return NULL;
#else
    MUST_MATCH_TOKEN(TOK_RC, JSMSG_CURLY_AFTER_BODY);
#endif
    pn->pn_pos.end = CURRENT_TOKEN(ts).pos.end;

#if JS_HAS_DESTRUCTURING
    /*
     * If there were destructuring formal parameters, prepend the initializing
     * comma expression that we synthesized to body.  If the body is a lexical
     * scope node, we must make a special TOK_SEQ node, to prepend the formal
     * parameter destructuring code without bracing the decompilation of the
     * function body's lexical scope.
     */
    if (list) {
        if (body->pn_arity != PN_LIST) {
            JSParseNode *block;

            block = NewParseNode(PN_LIST, tc);
            if (!block)
                return NULL;
            block->pn_type = TOK_SEQ;
            block->pn_pos = body->pn_pos;
            block->initList(body);

            body = block;
        }

        item = NewParseNode(PN_UNARY, tc);
        if (!item)
            return NULL;

        item->pn_type = TOK_SEMI;
        item->pn_pos.begin = item->pn_pos.end = body->pn_pos.begin;
        item->pn_kid = list;
        item->pn_next = body->pn_head;
        body->pn_head = item;
        if (body->pn_tail == &body->pn_head)
            body->pn_tail = &item->pn_next;
        ++body->pn_count;
        body->pn_xflags |= PNX_DESTRUCT;
    }
#endif

    /*
     * If we collected flags that indicate nested heavyweight functions, or
     * this function contains heavyweight-making statements (with statement,
     * visible eval call, or assignment to 'arguments'), flag the function as
     * heavyweight (requiring a call object per invocation).
     */
    if (funtc.flags & TCF_FUN_HEAVYWEIGHT) {
        fun->flags |= JSFUN_HEAVYWEIGHT;
        tc->flags |= TCF_FUN_HEAVYWEIGHT;
    } else {
        /*
         * If this function is a named statement function not at top-level
         * (i.e. not a top-level function definiton or expression), then our
         * enclosing function, if any, must be heavyweight.
         */
        if (!topLevel && lambda == 0 && funAtom)
            tc->flags |= TCF_FUN_HEAVYWEIGHT;
    }

    result = pn;
    if (lambda != 0) {
        /*
         * ECMA ed. 3 standard: function expression, possibly anonymous.
         */
        op = JSOP_LAMBDA;
    } else if (!funAtom) {
        /*
         * If this anonymous function definition is *not* embedded within a
         * larger expression, we treat it as an expression statement, not as
         * a function declaration -- and not as a syntax error (as ECMA-262
         * Edition 3 would have it).  Backward compatibility must trump all,
         * unless JSOPTION_ANONFUNFIX is set.
         */
        result = NewParseNode(PN_UNARY, tc);
        if (!result)
            return NULL;
        result->pn_type = TOK_SEMI;
        result->pn_pos = pn->pn_pos;
        result->pn_kid = pn;
        op = JSOP_LAMBDA;
    } else if (!topLevel) {
        /*
         * ECMA ed. 3 extension: a function expression statement not at the
         * top level, e.g., in a compound statement such as the "then" part
         * of an "if" statement, binds a closure only if control reaches that
         * sub-statement.
         */
        op = JSOP_DEFFUN;
    } else {
        op = JSOP_NOP;
    }

    funbox->kids = funtc.functionList;

    pn->pn_funbox = funbox;
    pn->pn_op = op;
    if (pn->pn_body) {
        pn->pn_body->append(body);
        pn->pn_body->pn_pos = body->pn_pos;
    } else {
        pn->pn_body = body;
    }

    pn->pn_blockid = tc->blockid();

    if (!LeaveFunction(pn, &funtc, tc, funAtom, lambda))
        return NULL;

    return result;
}

static JSParseNode *
FunctionStmt(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    return FunctionDef(cx, ts, tc, 0);
}

static JSParseNode *
FunctionExpr(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    return FunctionDef(cx, ts, tc, JSFUN_LAMBDA);
}

/*
 * Parse the statements in a block, creating a TOK_LC node that lists the
 * statements' trees.  If called from block-parsing code, the caller must
 * match { before and } after.
 */
static JSParseNode *
Statements(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSParseNode *pn, *pn2, *saveBlock;
    JSTokenType tt;

    JS_CHECK_RECURSION(cx, return NULL);

    pn = NewParseNode(PN_LIST, tc);
    if (!pn)
        return NULL;
    pn->pn_type = TOK_LC;
    pn->makeEmpty();
    pn->pn_blockid = tc->blockid();
    saveBlock = tc->blockNode;
    tc->blockNode = pn;

    for (;;) {
        ts->flags |= TSF_OPERAND;
        tt = js_PeekToken(cx, ts);
        ts->flags &= ~TSF_OPERAND;
        if (tt <= TOK_EOF || tt == TOK_RC) {
            if (tt == TOK_ERROR) {
                if (ts->flags & TSF_EOF)
                    ts->flags |= TSF_UNEXPECTED_EOF;
                return NULL;
            }
            break;
        }
        pn2 = Statement(cx, ts, tc);
        if (!pn2) {
            if (ts->flags & TSF_EOF)
                ts->flags |= TSF_UNEXPECTED_EOF;
            return NULL;
        }

        if (pn2->pn_type == TOK_FUNCTION) {
            /*
             * PNX_FUNCDEFS notifies the emitter that the block contains top-
             * level function definitions that should be processed before the
             * rest of nodes.
             *
             * TCF_HAS_FUNCTION_STMT is for the TOK_LC case in Statement. It
             * is relevant only for function definitions not at top-level,
             * which we call function statements.
             */
            if (tc->atTopLevel())
                pn->pn_xflags |= PNX_FUNCDEFS;
            else
                tc->flags |= TCF_HAS_FUNCTION_STMT;
        }
        pn->append(pn2);
    }

    /*
     * Handle the case where there was a let declaration under this block.  If
     * it replaced tc->blockNode with a new block node then we must refresh pn
     * and then restore tc->blockNode.
     */
    if (tc->blockNode != pn)
        pn = tc->blockNode;
    tc->blockNode = saveBlock;

    pn->pn_pos.end = CURRENT_TOKEN(ts).pos.end;
    return pn;
}

static JSParseNode *
Condition(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSParseNode *pn;

    MUST_MATCH_TOKEN(TOK_LP, JSMSG_PAREN_BEFORE_COND);
    pn = ParenExpr(cx, ts, tc, NULL, NULL);
    if (!pn)
        return NULL;
    MUST_MATCH_TOKEN(TOK_RP, JSMSG_PAREN_AFTER_COND);

    /* Check for (a = b) and warn about possible (a == b) mistype. */
    if (pn->pn_type == TOK_ASSIGN &&
        pn->pn_op == JSOP_NOP &&
        !pn->pn_parens &&
        !js_ReportCompileErrorNumber(cx, ts, NULL,
                                     JSREPORT_WARNING | JSREPORT_STRICT,
                                     JSMSG_EQUAL_AS_ASSIGN,
                                     "")) {
        return NULL;
    }
    return pn;
}

static JSBool
MatchLabel(JSContext *cx, JSTokenStream *ts, JSParseNode *pn)
{
    JSAtom *label;
    JSTokenType tt;

    tt = js_PeekTokenSameLine(cx, ts);
    if (tt == TOK_ERROR)
        return JS_FALSE;
    if (tt == TOK_NAME) {
        (void) js_GetToken(cx, ts);
        label = CURRENT_TOKEN(ts).t_atom;
    } else {
        label = NULL;
    }
    pn->pn_atom = label;
    return JS_TRUE;
}

static JSBool
BindLet(JSContext *cx, BindData *data, JSAtom *atom, JSTreeContext *tc)
{
    JSParseNode *pn;
    JSObject *blockObj;
    JSAtomListElement *ale;
    jsint n;

    /*
     * Top-level 'let' is the same as 'var' currently -- this may change in a
     * successor standard to ES3.1 that specifies 'let'.
     */
    JS_ASSERT(!tc->atTopLevel());

    pn = data->pn;
    blockObj = tc->blockChain;
    ale = tc->decls.lookup(atom);
    if (ale && ALE_DEFN(ale)->pn_blockid == tc->blockid()) {
        const char *name = js_AtomToPrintableString(cx, atom);
        if (name) {
            js_ReportCompileErrorNumber(cx, TS(tc->compiler), pn,
                                        JSREPORT_ERROR, JSMSG_REDECLARED_VAR,
                                        (ale && ALE_DEFN(ale)->isConst())
                                        ? js_const_str
                                        : js_variable_str,
                                        name);
        }
        return JS_FALSE;
    }

    n = OBJ_BLOCK_COUNT(cx, blockObj);
    if (n == JS_BIT(16)) {
        js_ReportCompileErrorNumber(cx, TS(tc->compiler), pn,
                                    JSREPORT_ERROR, data->let.overflow);
        return JS_FALSE;
    }

    /*
     * Pass push = true to Define so it pushes an ale ahead of any outer scope.
     * This is balanced by PopStatement, defined immediately below.
     */
    if (!Define(pn, atom, tc, true))
        return JS_FALSE;

    /*
     * Assign block-local index to pn->pn_cookie right away, encoding it as an
     * upvar cookie whose skip tells the current static level. The emitter will
     * adjust the node's slot based on its stack depth model -- and, for global
     * and eval code, JSCompiler::compileScript will adjust the slot again to
     * include script->nfixed.
     */
    pn->pn_op = JSOP_GETLOCAL;
    pn->pn_cookie = MAKE_UPVAR_COOKIE(tc->staticLevel, n);
    pn->pn_dflags |= PND_LET | PND_BOUND;

    /*
     * Define the let binding's property before storing pn in a reserved slot,
     * since block_reserveSlots depends on OBJ_SCOPE(blockObj)->entryCount.
     */
    if (!js_DefineBlockVariable(cx, blockObj, ATOM_TO_JSID(atom), n))
        return JS_FALSE;

    /*
     * Store pn temporarily in what would be reserved slots in a cloned block
     * object (once the prototype's final population is known, after all 'let'
     * bindings for this block have been parsed). We will free these reserved
     * slots in jsemit.cpp:EmitEnterBlock.
     */
    uintN slot = JSSLOT_FREE(&js_BlockClass) + n;
    if (slot >= STOBJ_NSLOTS(blockObj) &&
        !js_GrowSlots(cx, blockObj, slot + 1)) {
        return JS_FALSE;
    }
    OBJ_SCOPE(blockObj)->freeslot = slot + 1;
    STOBJ_SET_SLOT(blockObj, slot, PRIVATE_TO_JSVAL(pn));
    return JS_TRUE;
}

static void
PopStatement(JSTreeContext *tc)
{
    JSStmtInfo *stmt = tc->topStmt;

    if (stmt->flags & SIF_SCOPE) {
        JSObject *obj = stmt->blockObj;
        JSScope *scope = OBJ_SCOPE(obj);
        JS_ASSERT(!OBJ_IS_CLONED_BLOCK(obj));

        for (JSScopeProperty *sprop = scope->lastProp; sprop; sprop = sprop->parent) {
            JSAtom *atom = JSID_TO_ATOM(sprop->id);

            /* Beware the empty destructuring dummy. */
            if (atom == tc->compiler->context->runtime->atomState.emptyAtom)
                continue;
            tc->decls.remove(tc->compiler, atom);
        }

        /*
         * The block scope will not be modified again. It may be shared. Clear
         * scope->object to make scope->owned() false.
         */
        scope->object = NULL;
    }
    js_PopStatement(tc);
}

static inline bool
OuterLet(JSTreeContext *tc, JSStmtInfo *stmt, JSAtom *atom)
{
    while (stmt->downScope) {
        stmt = js_LexicalLookup(tc, atom, NULL, stmt->downScope);
        if (!stmt)
            return false;
        if (stmt->type == STMT_BLOCK)
            return true;
    }
    return false;
}

static JSBool
BindVarOrConst(JSContext *cx, BindData *data, JSAtom *atom, JSTreeContext *tc)
{
    JSStmtInfo *stmt = js_LexicalLookup(tc, atom, NULL);
    JSParseNode *pn = data->pn;

    if (stmt && stmt->type == STMT_WITH) {
        pn->pn_op = JSOP_NAME;
        data->fresh = false;
        return JS_TRUE;
    }

    JSAtomListElement *ale = tc->decls.lookup(atom);
    JSOp op = data->op;

    if (stmt || ale) {
        JSDefinition *dn = ale ? ALE_DEFN(ale) : NULL;
        JSDefinition::Kind dn_kind = dn ? dn->kind() : JSDefinition::VAR;
        const char *name;

        if (dn_kind == JSDefinition::ARG) {
            name = js_AtomToPrintableString(cx, atom);
            if (!name)
                return JS_FALSE;

            if (op == JSOP_DEFCONST) {
                js_ReportCompileErrorNumber(cx, TS(tc->compiler), pn,
                                            JSREPORT_ERROR, JSMSG_REDECLARED_PARAM,
                                            name);
                return JS_FALSE;
            }
            if (!js_ReportCompileErrorNumber(cx, TS(tc->compiler), pn,
                                             JSREPORT_WARNING | JSREPORT_STRICT,
                                             JSMSG_VAR_HIDES_ARG, name)) {
                return JS_FALSE;
            }
        } else {
            bool error = (op == JSOP_DEFCONST ||
                          dn_kind == JSDefinition::CONST ||
                          (dn_kind == JSDefinition::LET &&
                           (stmt->type != STMT_CATCH || OuterLet(tc, stmt, atom))));

            if (JS_HAS_STRICT_OPTION(cx)
                ? op != JSOP_DEFVAR || dn_kind != JSDefinition::VAR
                : error) {
                name = js_AtomToPrintableString(cx, atom);
                if (!name ||
                    !js_ReportCompileErrorNumber(cx, TS(tc->compiler), pn,
                                                 !error
                                                 ? JSREPORT_WARNING | JSREPORT_STRICT
                                                 : JSREPORT_ERROR,
                                                 JSMSG_REDECLARED_VAR,
                                                 JSDefinition::kindString(dn_kind),
                                                 name)) {
                    return JS_FALSE;
                }
            }
        }
    }

    if (!ale) {
        if (!Define(pn, atom, tc))
            return JS_FALSE;
    } else {
        /*
         * A var declaration never recreates an existing binding, it restates
         * it and possibly reinitializes its value. Beware that if pn becomes a
         * use of ALE_DEFN(ale), and if we have an initializer for this var or
         * const (typically a const would ;-), then pn must be rewritten into a
         * TOK_ASSIGN node. See Variables, further below.
         *
         * A case such as let (x = 1) { var x = 2; print(x); } is even harder.
         * There the x definition is hoisted but the x = 2 assignment mutates
         * the block-local binding of x.
         */
        JSDefinition *dn = ALE_DEFN(ale);

        data->fresh = false;

        if (!pn->pn_used) {
            /* Make pnu be a fresh name node that uses dn. */
            JSParseNode *pnu = pn;

            if (pn->pn_defn) {
                pnu = NewNameNode(cx, atom, tc);
                if (!pnu)
                    return JS_FALSE;
            }

            LinkUseToDef(pnu, dn, tc);
            pnu->pn_op = JSOP_NAME;
        }

        while (dn->kind() == JSDefinition::LET) {
            do {
                ale = ALE_NEXT(ale);
            } while (ale && ALE_ATOM(ale) != atom);
            if (!ale)
                break;
            dn = ALE_DEFN(ale);
        }

        if (ale) {
            JS_ASSERT_IF(data->op == JSOP_DEFCONST,
                         dn->kind() == JSDefinition::CONST);
            return JS_TRUE;
        }

        /*
         * A var or const that is shadowed by one or more let bindings of the
         * same name, but that has not been declared until this point, must be
         * hoisted above the let bindings.
         */
        if (!pn->pn_defn) {
            JSHashEntry **hep;

            ale = tc->lexdeps.rawLookup(atom, hep);
            if (ale) {
                pn = ALE_DEFN(ale);
                tc->lexdeps.rawRemove(tc->compiler, ale, hep);
            } else {
                JSParseNode *pn2 = NewNameNode(cx, atom, tc);
                if (!pn2)
                    return JS_FALSE;

                /* The token stream may be past the location for pn. */
                pn2->pn_type = TOK_NAME;
                pn2->pn_pos = pn->pn_pos;
                pn = pn2;
            }
            pn->pn_op = JSOP_NAME;
        }

        ale = tc->decls.add(tc->compiler, atom, JSAtomList::HOIST);
        if (!ale)
            return JS_FALSE;
        ALE_SET_DEFN(ale, pn);
        pn->pn_defn = true;
        pn->pn_dflags &= ~PND_PLACEHOLDER;
    }

    if (data->op == JSOP_DEFCONST)
        pn->pn_dflags |= PND_CONST;

    if (!(tc->flags & TCF_IN_FUNCTION)) {
        /*
         * If we are generating global or eval-called-from-global code, bind a
         * "gvar" here, as soon as possible. The JSOP_GETGVAR, etc., ops speed
         * up global variable access by memoizing name-to-slot mappings in the
         * script prolog (via JSOP_DEFVAR/JSOP_DEFCONST). If the memoization
         * can't be done due to a pre-existing property of the same name as the
         * var or const but incompatible attributes/getter/setter/etc, these
         * ops devolve to JSOP_NAME, etc.
         *
         * For now, don't try to lookup eval frame variables at compile time.
         * Seems sub-optimal: why couldn't we find eval-called-from-a-function
         * upvars early and possibly simplify jsemit.cpp:BindNameToSlot?
         */
        pn->pn_op = JSOP_NAME;
        if ((tc->flags & TCF_COMPILING) && !tc->compiler->callerFrame) {
            JSCodeGenerator *cg = (JSCodeGenerator *) tc;

            /* Index atom so we can map fast global number to name. */
            ale = cg->atomList.add(tc->compiler, atom);
            if (!ale)
                return JS_FALSE;

            /* Defend against cg->ngvars 16-bit overflow. */
            uintN slot = ALE_INDEX(ale);
            if ((slot + 1) >> 16)
                return JS_TRUE;

            if ((uint16)(slot + 1) > cg->ngvars)
                cg->ngvars = (uint16)(slot + 1);

            pn->pn_op = JSOP_GETGVAR;
            pn->pn_cookie = MAKE_UPVAR_COOKIE(tc->staticLevel, slot);
            pn->pn_dflags |= PND_BOUND | PND_GVAR;
        }
        return JS_TRUE;
    }

    if (atom == cx->runtime->atomState.argumentsAtom) {
        pn->pn_op = JSOP_ARGUMENTS;
        pn->pn_dflags |= PND_BOUND;
        return JS_TRUE;
    }

    JSLocalKind localKind = js_LookupLocal(cx, tc->fun, atom, NULL);
    if (localKind == JSLOCAL_NONE) {
        /*
         * Property not found in current variable scope: we have not seen this
         * variable before. Define a new local variable by adding a property to
         * the function's scope and allocating one slot in the function's vars
         * frame. Any locals declared in a with statement body are handled at
         * runtime, by script prolog JSOP_DEFVAR opcodes generated for global
         * and heavyweight-function-local vars.
         */
        localKind = (data->op == JSOP_DEFCONST) ? JSLOCAL_CONST : JSLOCAL_VAR;

        uintN index = tc->fun->u.i.nvars;
        if (!BindLocalVariable(cx, tc->fun, atom, localKind))
            return JS_FALSE;
        pn->pn_op = JSOP_GETLOCAL;
        pn->pn_cookie = MAKE_UPVAR_COOKIE(tc->staticLevel, index);
        pn->pn_dflags |= PND_BOUND;
        return JS_TRUE;
    }

    if (localKind == JSLOCAL_ARG) {
        /* We checked errors and strict warnings earlier -- see above. */
        JS_ASSERT(ale && ALE_DEFN(ale)->kind() == JSDefinition::ARG);
    } else {
        /* Not an argument, must be a redeclared local var. */
        JS_ASSERT(localKind == JSLOCAL_VAR || localKind == JSLOCAL_CONST);
    }
    pn->pn_op = JSOP_NAME;
    return JS_TRUE;
}

static JSBool
MakeSetCall(JSContext *cx, JSParseNode *pn, JSTreeContext *tc, uintN msg)
{
    JSParseNode *pn2;

    JS_ASSERT(pn->pn_arity == PN_LIST);
    JS_ASSERT(pn->pn_op == JSOP_CALL || pn->pn_op == JSOP_EVAL || pn->pn_op == JSOP_APPLY);
    pn2 = pn->pn_head;
    if (pn2->pn_type == TOK_FUNCTION && (pn2->pn_funbox->tcflags & TCF_GENEXP_LAMBDA)) {
        js_ReportCompileErrorNumber(cx, TS(tc->compiler), pn, JSREPORT_ERROR, msg);
        return JS_FALSE;
    }
    pn->pn_op = JSOP_SETCALL;
    return JS_TRUE;
}

static void
NoteLValue(JSContext *cx, JSParseNode *pn, JSTreeContext *tc, uintN dflag = PND_ASSIGNED)
{
    if (pn->pn_used) {
        JSDefinition *dn = pn->pn_lexdef;

        /*
         * Save the win of PND_INITIALIZED if we can prove 'var x;' and 'x = y'
         * occur as direct kids of the same block with no forward refs to x.
         */
        if (!(dn->pn_dflags & (PND_INITIALIZED | PND_CONST | PND_PLACEHOLDER)) &&
            dn->isBlockChild() &&
            pn->isBlockChild() &&
            dn->pn_blockid == pn->pn_blockid &&
            dn->pn_pos.end <= pn->pn_pos.begin &&
            dn->dn_uses == pn) {
            dflag = PND_INITIALIZED;
        }

        dn->pn_dflags |= dflag;

        if (dn->frameLevel() != tc->staticLevel) {
            /*
             * The above condition takes advantage of the all-ones nature of
             * FREE_UPVAR_COOKIE, and the reserved level FREE_STATIC_LEVEL.
             * We make a stronger assertion by excluding FREE_UPVAR_COOKIE.
             */
            JS_ASSERT_IF(dn->pn_cookie != FREE_UPVAR_COOKIE,
                         dn->frameLevel() < tc->staticLevel);
            tc->flags |= TCF_FUN_SETS_OUTER_NAME;
        }
    }

    pn->pn_dflags |= dflag;

    if (pn->pn_atom == cx->runtime->atomState.argumentsAtom)
        tc->flags |= TCF_FUN_HEAVYWEIGHT;
}

#if JS_HAS_DESTRUCTURING

static JSBool
BindDestructuringVar(JSContext *cx, BindData *data, JSParseNode *pn,
                     JSTreeContext *tc)
{
    JSAtom *atom;

    /*
     * Destructuring is a form of assignment, so just as for an initialized
     * simple variable, we must check for assignment to 'arguments' and flag
     * the enclosing function (if any) as heavyweight.
     */
    JS_ASSERT(pn->pn_type == TOK_NAME);
    atom = pn->pn_atom;
    if (atom == cx->runtime->atomState.argumentsAtom)
        tc->flags |= TCF_FUN_HEAVYWEIGHT;

    data->pn = pn;
    if (!data->binder(cx, data, atom, tc))
        return JS_FALSE;

    /*
     * Select the appropriate name-setting opcode, respecting eager selection
     * done by the data->binder function.
     */
    if (pn->pn_dflags & PND_BOUND) {
        pn->pn_op = (pn->pn_op == JSOP_ARGUMENTS)
                    ? JSOP_SETNAME
                    : (pn->pn_dflags & PND_GVAR)
                    ? JSOP_SETGVAR
                    : JSOP_SETLOCAL;
    } else {
        pn->pn_op = (data->op == JSOP_DEFCONST)
                    ? JSOP_SETCONST
                    : JSOP_SETNAME;
    }

    if (data->op == JSOP_DEFCONST)
        pn->pn_dflags |= PND_CONST;

    NoteLValue(cx, pn, tc, PND_INITIALIZED);
    return JS_TRUE;
}

/*
 * Here, we are destructuring {... P: Q, ...} = R, where P is any id, Q is any
 * LHS expression except a destructuring initialiser, and R is on the stack.
 * Because R is already evaluated, the usual LHS-specialized bytecodes won't
 * work.  After pushing R[P] we need to evaluate Q's "reference base" QB and
 * then push its property name QN.  At this point the stack looks like
 *
 *   [... R, R[P], QB, QN]
 *
 * We need to set QB[QN] = R[P].  This is a job for JSOP_ENUMELEM, which takes
 * its operands with left-hand side above right-hand side:
 *
 *   [rval, lval, xval]
 *
 * and pops all three values, setting lval[xval] = rval.  But we cannot select
 * JSOP_ENUMELEM yet, because the LHS may turn out to be an arg or local var,
 * which can be optimized further.  So we select JSOP_SETNAME.
 */
static JSBool
BindDestructuringLHS(JSContext *cx, JSParseNode *pn, JSTreeContext *tc)
{
    switch (pn->pn_type) {
      case TOK_NAME:
        NoteLValue(cx, pn, tc);
        /* FALL THROUGH */

      case TOK_DOT:
      case TOK_LB:
        pn->pn_op = JSOP_SETNAME;
        break;

      case TOK_LP:
        if (!MakeSetCall(cx, pn, tc, JSMSG_BAD_LEFTSIDE_OF_ASS))
            return JS_FALSE;
        break;

#if JS_HAS_XML_SUPPORT
      case TOK_UNARYOP:
        if (pn->pn_op == JSOP_XMLNAME) {
            pn->pn_op = JSOP_BINDXMLNAME;
            break;
        }
        /* FALL THROUGH */
#endif

      default:
        js_ReportCompileErrorNumber(cx, TS(tc->compiler), pn,
                                    JSREPORT_ERROR, JSMSG_BAD_LEFTSIDE_OF_ASS);
        return JS_FALSE;
    }

    return JS_TRUE;
}

typedef struct FindPropValData {
    uint32          numvars;    /* # of destructuring vars in left side */
    uint32          maxstep;    /* max # of steps searching right side */
    JSDHashTable    table;      /* hash table for O(1) right side search */
} FindPropValData;

typedef struct FindPropValEntry {
    JSDHashEntryHdr hdr;
    JSParseNode     *pnkey;
    JSParseNode     *pnval;
} FindPropValEntry;

#define ASSERT_VALID_PROPERTY_KEY(pnkey)                                      \
    JS_ASSERT(((pnkey)->pn_arity == PN_NULLARY &&                             \
               ((pnkey)->pn_type == TOK_NUMBER ||                             \
                (pnkey)->pn_type == TOK_STRING ||                             \
                (pnkey)->pn_type == TOK_NAME)) ||                             \
               ((pnkey)->pn_arity == PN_NAME && (pnkey)->pn_type == TOK_NAME))

static JSDHashNumber
HashFindPropValKey(JSDHashTable *table, const void *key)
{
    const JSParseNode *pnkey = (const JSParseNode *)key;

    ASSERT_VALID_PROPERTY_KEY(pnkey);
    return (pnkey->pn_type == TOK_NUMBER)
           ? (JSDHashNumber) JS_HASH_DOUBLE(pnkey->pn_dval)
           : ATOM_HASH(pnkey->pn_atom);
}

static JSBool
MatchFindPropValEntry(JSDHashTable *table,
                      const JSDHashEntryHdr *entry,
                      const void *key)
{
    const FindPropValEntry *fpve = (const FindPropValEntry *)entry;
    const JSParseNode *pnkey = (const JSParseNode *)key;

    ASSERT_VALID_PROPERTY_KEY(pnkey);
    return pnkey->pn_type == fpve->pnkey->pn_type &&
           ((pnkey->pn_type == TOK_NUMBER)
            ? pnkey->pn_dval == fpve->pnkey->pn_dval
            : pnkey->pn_atom == fpve->pnkey->pn_atom);
}

static const JSDHashTableOps FindPropValOps = {
    JS_DHashAllocTable,
    JS_DHashFreeTable,
    HashFindPropValKey,
    MatchFindPropValEntry,
    JS_DHashMoveEntryStub,
    JS_DHashClearEntryStub,
    JS_DHashFinalizeStub,
    NULL
};

#define STEP_HASH_THRESHOLD     10
#define BIG_DESTRUCTURING        5
#define BIG_OBJECT_INIT         20

static JSParseNode *
FindPropertyValue(JSParseNode *pn, JSParseNode *pnid, FindPropValData *data)
{
    FindPropValEntry *entry;
    JSParseNode *pnhit, *pnhead, *pnprop, *pnkey;
    uint32 step;

    /* If we have a hash table, use it as the sole source of truth. */
    if (data->table.ops) {
        entry = (FindPropValEntry *)
                JS_DHashTableOperate(&data->table, pnid, JS_DHASH_LOOKUP);
        return JS_DHASH_ENTRY_IS_BUSY(&entry->hdr) ? entry->pnval : NULL;
    }

    /* If pn is not an object initialiser node, we can't do anything here. */
    if (pn->pn_type != TOK_RC)
        return NULL;

    /*
     * We must search all the way through pn's list, to handle the case of an
     * id duplicated for two or more property initialisers.
     */
    pnhit = NULL;
    step = 0;
    ASSERT_VALID_PROPERTY_KEY(pnid);
    pnhead = pn->pn_head;
    if (pnid->pn_type == TOK_NUMBER) {
        for (pnprop = pnhead; pnprop; pnprop = pnprop->pn_next) {
            JS_ASSERT(pnprop->pn_type == TOK_COLON);
            if (pnprop->pn_op == JSOP_NOP) {
                pnkey = pnprop->pn_left;
                ASSERT_VALID_PROPERTY_KEY(pnkey);
                if (pnkey->pn_type == TOK_NUMBER &&
                    pnkey->pn_dval == pnid->pn_dval) {
                    pnhit = pnprop;
                }
                ++step;
            }
        }
    } else {
        for (pnprop = pnhead; pnprop; pnprop = pnprop->pn_next) {
            JS_ASSERT(pnprop->pn_type == TOK_COLON);
            if (pnprop->pn_op == JSOP_NOP) {
                pnkey = pnprop->pn_left;
                ASSERT_VALID_PROPERTY_KEY(pnkey);
                if (pnkey->pn_type == pnid->pn_type &&
                    pnkey->pn_atom == pnid->pn_atom) {
                    pnhit = pnprop;
                }
                ++step;
            }
        }
    }
    if (!pnhit)
        return NULL;

    /* Hit via full search -- see whether it's time to create the hash table. */
    JS_ASSERT(!data->table.ops);
    if (step > data->maxstep) {
        data->maxstep = step;
        if (step >= STEP_HASH_THRESHOLD &&
            data->numvars >= BIG_DESTRUCTURING &&
            pn->pn_count >= BIG_OBJECT_INIT &&
            JS_DHashTableInit(&data->table, &FindPropValOps, pn,
                              sizeof(FindPropValEntry),
                              JS_DHASH_DEFAULT_CAPACITY(pn->pn_count)))
        {
            for (pn = pnhead; pn; pn = pn->pn_next) {
                JS_ASSERT(pnprop->pn_type == TOK_COLON);
                ASSERT_VALID_PROPERTY_KEY(pn->pn_left);
                entry = (FindPropValEntry *)
                        JS_DHashTableOperate(&data->table, pn->pn_left,
                                             JS_DHASH_ADD);
                entry->pnval = pn->pn_right;
            }
        }
    }
    return pnhit->pn_right;
}

/*
 * Destructuring patterns can appear in two kinds of contexts:
 *
 * - assignment-like: assignment expressions and |for| loop heads.  In
 *   these cases, the patterns' property value positions can be
 *   arbitrary lvalue expressions; the destructuring is just a fancy
 *   assignment.
 *
 * - declaration-like: |var| and |let| declarations, functions' formal
 *   parameter lists, |catch| clauses, and comprehension tails.  In
 *   these cases, the patterns' property value positions must be
 *   simple names; the destructuring defines them as new variables.
 *
 * In both cases, other code parses the pattern as an arbitrary
 * PrimaryExpr, and then, here in CheckDestructuring, verify that the
 * tree is a valid destructuring expression.
 *
 * In assignment-like contexts, we parse the pattern with the
 * TCF_DECL_DESTRUCTURING flag clear, so the lvalue expressions in the
 * pattern are parsed normally.  PrimaryExpr links variable references
 * into the appropriate use chains; creates placeholder definitions;
 * and so on.  CheckDestructuring is called with |data| NULL (since we
 * won't be binding any new names), and we specialize lvalues as
 * appropriate.  If right is NULL, we just check for well-formed lvalues.
 *
 * In declaration-like contexts, the normal variable reference
 * processing would just be an obstruction, because we're going to
 * define the names that appear in the property value positions as new
 * variables anyway.  In this case, we parse the pattern with
 * TCF_DECL_DESTRUCTURING set, which directs PrimaryExpr to leave
 * whatever name nodes it creates unconnected.  Then, here in
 * CheckDestructuring, we require the pattern's property value
 * positions to be simple names, and define them as appropriate to the
 * context.  For these calls, |data| points to the right sort of
 * BindData.
 *
 * See also UndominateInitializers, immediately below. If you change
 * either of these functions, you might have to change the other to
 * match.
 */
static JSBool
CheckDestructuring(JSContext *cx, BindData *data,
                   JSParseNode *left, JSParseNode *right,
                   JSTreeContext *tc)
{
    JSBool ok;
    FindPropValData fpvd;
    JSParseNode *lhs, *rhs, *pn, *pn2;

    if (left->pn_type == TOK_ARRAYCOMP) {
        js_ReportCompileErrorNumber(cx, TS(tc->compiler), left,
                                    JSREPORT_ERROR, JSMSG_ARRAY_COMP_LEFTSIDE);
        return JS_FALSE;
    }

#if JS_HAS_DESTRUCTURING_SHORTHAND
    if (right && right->pn_arity == PN_LIST && (right->pn_xflags & PNX_DESTRUCT)) {
        js_ReportCompileErrorNumber(cx, TS(tc->compiler), right,
                                    JSREPORT_ERROR, JSMSG_BAD_OBJECT_INIT);
        return JS_FALSE;
    }
#endif

    fpvd.table.ops = NULL;
    lhs = left->pn_head;
    if (left->pn_type == TOK_RB) {
        rhs = (right && right->pn_type == left->pn_type)
              ? right->pn_head
              : NULL;

        while (lhs) {
            pn = lhs, pn2 = rhs;

            /* Nullary comma is an elision; binary comma is an expression.*/
            if (pn->pn_type != TOK_COMMA || pn->pn_arity != PN_NULLARY) {
                if (pn->pn_type == TOK_RB || pn->pn_type == TOK_RC) {
                    ok = CheckDestructuring(cx, data, pn, pn2, tc);
                } else {
                    if (data) {
                        if (pn->pn_type != TOK_NAME)
                            goto no_var_name;

                        ok = BindDestructuringVar(cx, data, pn, tc);
                    } else {
                        ok = BindDestructuringLHS(cx, pn, tc);
                    }
                }
                if (!ok)
                    goto out;
            }

            lhs = lhs->pn_next;
            if (rhs)
                rhs = rhs->pn_next;
        }
    } else {
        JS_ASSERT(left->pn_type == TOK_RC);
        fpvd.numvars = left->pn_count;
        fpvd.maxstep = 0;
        rhs = NULL;

        while (lhs) {
            JS_ASSERT(lhs->pn_type == TOK_COLON);
            pn = lhs->pn_right;

            if (pn->pn_type == TOK_RB || pn->pn_type == TOK_RC) {
                if (right)
                    rhs = FindPropertyValue(right, lhs->pn_left, &fpvd);
                ok = CheckDestructuring(cx, data, pn, rhs, tc);
            } else if (data) {
                if (pn->pn_type != TOK_NAME)
                    goto no_var_name;

                ok = BindDestructuringVar(cx, data, pn, tc);
            } else {
                ok = BindDestructuringLHS(cx, pn, tc);
            }
            if (!ok)
                goto out;

            lhs = lhs->pn_next;
        }
    }

    /*
     * The catch/finally handler implementation in the interpreter assumes
     * that any operation that introduces a new scope (like a "let" or "with"
     * block) increases the stack depth. This way, it is possible to restore
     * the scope chain based on stack depth of the handler alone. "let" with
     * an empty destructuring pattern like in
     *
     *   let [] = 1;
     *
     * would violate this assumption as the there would be no let locals to
     * store on the stack. To satisfy it we add an empty property to such
     * blocks so that OBJ_BLOCK_COUNT(cx, blockObj), which gives the number of
     * slots, would be always positive.
     *
     * Note that we add such a property even if the block has locals due to
     * later let declarations in it. We optimize for code simplicity here,
     * not the fastest runtime performance with empty [] or {}.
     */
    if (data &&
        data->binder == BindLet &&
        OBJ_BLOCK_COUNT(cx, tc->blockChain) == 0) {
        ok = !!js_DefineNativeProperty(cx, tc->blockChain,
                                       ATOM_TO_JSID(cx->runtime->
                                                    atomState.emptyAtom),
                                       JSVAL_VOID, NULL, NULL,
                                       JSPROP_ENUMERATE |
                                       JSPROP_PERMANENT |
                                       JSPROP_SHARED,
                                       SPROP_HAS_SHORTID, 0, NULL);
        if (!ok)
            goto out;
    }

    ok = JS_TRUE;

  out:
    if (fpvd.table.ops)
        JS_DHashTableFinish(&fpvd.table);
    return ok;

  no_var_name:
    js_ReportCompileErrorNumber(cx, TS(tc->compiler), pn, JSREPORT_ERROR,
                                JSMSG_NO_VARIABLE_NAME);
    ok = JS_FALSE;
    goto out;
}

/*
 * This is a greatly pared down version of CheckDestructuring that extends the
 * pn_pos.end source coordinate of each name in a destructuring binding such as
 *
 *   var [x, y] = [function () y, 42];
 *
 * to cover its corresponding initializer, so that the initialized binding does
 * not appear to dominate any closures in its initializer. See bug 496134.
 *
 * The quick-and-dirty dominance computation in JSCompiler::setFunctionKinds is
 * not very precise. With one-pass SSA construction from structured source code
 * (see "Single-Pass Generation of Static Single Assignment Form for Structured
 * Languages", Brandis and Mössenböck), we could do much better.
 *
 * See CheckDestructuring, immediately above. If you change either of these
 * functions, you might have to change the other to match.
 */
static JSBool
UndominateInitializers(JSParseNode *left, JSParseNode *right, JSTreeContext *tc)
{
    FindPropValData fpvd;
    JSParseNode *lhs, *rhs;

    JS_ASSERT(left->pn_type != TOK_ARRAYCOMP);
    JS_ASSERT(right);

#if JS_HAS_DESTRUCTURING_SHORTHAND
    if (right->pn_arity == PN_LIST && (right->pn_xflags & PNX_DESTRUCT)) {
        js_ReportCompileErrorNumber(tc->compiler->context, TS(tc->compiler), right,
                                    JSREPORT_ERROR, JSMSG_BAD_OBJECT_INIT);
        return JS_FALSE;
    }
#endif

    if (right->pn_type != left->pn_type)
        return JS_TRUE;

    fpvd.table.ops = NULL;
    lhs = left->pn_head;
    if (left->pn_type == TOK_RB) {
        rhs = right->pn_head;

        while (lhs && rhs) {
            /* Nullary comma is an elision; binary comma is an expression.*/
            if (lhs->pn_type != TOK_COMMA || lhs->pn_arity != PN_NULLARY) {
                if (lhs->pn_type == TOK_RB || lhs->pn_type == TOK_RC) {
                    if (!UndominateInitializers(lhs, rhs, tc))
                        return JS_FALSE;
                } else {
                    lhs->pn_pos.end = rhs->pn_pos.end;
                }
            }

            lhs = lhs->pn_next;
            rhs = rhs->pn_next;
        }
    } else {
        JS_ASSERT(left->pn_type == TOK_RC);
        fpvd.numvars = left->pn_count;
        fpvd.maxstep = 0;

        while (lhs) {
            JS_ASSERT(lhs->pn_type == TOK_COLON);
            JSParseNode *pn = lhs->pn_right;

            rhs = FindPropertyValue(right, lhs->pn_left, &fpvd);
            if (pn->pn_type == TOK_RB || pn->pn_type == TOK_RC) {
                if (rhs && !UndominateInitializers(pn, rhs, tc))
                    return JS_FALSE;
            } else {
                if (rhs)
                    pn->pn_pos.end = rhs->pn_pos.end;
            }

            lhs = lhs->pn_next;
        }
    }
    return JS_TRUE;
}

static JSParseNode *
DestructuringExpr(JSContext *cx, BindData *data, JSTreeContext *tc,
                  JSTokenType tt)
{
    JSTokenStream *ts;
    JSParseNode *pn;

    ts = TS(tc->compiler);
    tc->flags |= TCF_DECL_DESTRUCTURING;
    pn = PrimaryExpr(cx, ts, tc, tt, JS_FALSE);
    tc->flags &= ~TCF_DECL_DESTRUCTURING;
    if (!pn)
        return NULL;
    if (!CheckDestructuring(cx, data, pn, NULL, tc))
        return NULL;
    return pn;
}

/*
 * Currently used only #if JS_HAS_DESTRUCTURING, in Statement's TOK_FOR case.
 * This function assumes the cloned tree is for use in the same statement and
 * binding context as the original tree.
 */
static JSParseNode *
CloneParseTree(JSParseNode *opn, JSTreeContext *tc)
{
    JSParseNode *pn, *pn2, *opn2;

    pn = NewOrRecycledNode(tc);
    if (!pn)
        return NULL;
    pn->pn_type = opn->pn_type;
    pn->pn_pos = opn->pn_pos;
    pn->pn_op = opn->pn_op;
    pn->pn_used = opn->pn_used;
    pn->pn_defn = opn->pn_defn;
    pn->pn_arity = opn->pn_arity;
    pn->pn_parens = opn->pn_parens;

    switch (pn->pn_arity) {
#define NULLCHECK(e)    JS_BEGIN_MACRO if (!(e)) return NULL; JS_END_MACRO

      case PN_FUNC:
        NULLCHECK(pn->pn_funbox =
                  tc->compiler->newFunctionBox(opn->pn_funbox->object, pn, tc));
        NULLCHECK(pn->pn_body = CloneParseTree(opn->pn_body, tc));
        pn->pn_cookie = opn->pn_cookie;
        pn->pn_dflags = opn->pn_dflags;
        pn->pn_blockid = opn->pn_blockid;
        break;

      case PN_LIST:
        pn->makeEmpty();
        for (opn2 = opn->pn_head; opn2; opn2 = opn2->pn_next) {
            NULLCHECK(pn2 = CloneParseTree(opn2, tc));
            pn->append(pn2);
        }
        pn->pn_xflags = opn->pn_xflags;
        break;

      case PN_TERNARY:
        NULLCHECK(pn->pn_kid1 = CloneParseTree(opn->pn_kid1, tc));
        NULLCHECK(pn->pn_kid2 = CloneParseTree(opn->pn_kid2, tc));
        NULLCHECK(pn->pn_kid3 = CloneParseTree(opn->pn_kid3, tc));
        break;

      case PN_BINARY:
        NULLCHECK(pn->pn_left = CloneParseTree(opn->pn_left, tc));
        if (opn->pn_right != opn->pn_left)
            NULLCHECK(pn->pn_right = CloneParseTree(opn->pn_right, tc));
        else
            pn->pn_right = pn->pn_left;
        pn->pn_val = opn->pn_val;
        pn->pn_iflags = opn->pn_iflags;
        break;

      case PN_UNARY:
        NULLCHECK(pn->pn_kid = CloneParseTree(opn->pn_kid, tc));
        pn->pn_num = opn->pn_num;
        pn->pn_hidden = opn->pn_hidden;
        break;

      case PN_NAME:
        // PN_NAME could mean several arms in pn_u, so copy the whole thing.
        pn->pn_u = opn->pn_u;
        if (opn->pn_used) {
            /*
             * The old name is a use of its pn_lexdef. Make the clone also be a
             * use of that definition.
             */
            JSDefinition *dn = pn->pn_lexdef;

            pn->pn_link = dn->dn_uses;
            dn->dn_uses = pn;
        } else if (opn->pn_expr) {
            NULLCHECK(pn->pn_expr = CloneParseTree(opn->pn_expr, tc));

            /*
             * If the old name is a definition, the new one has pn_defn set.
             * Make the old name a use of the new node.
             */
            if (opn->pn_defn) {
                opn->pn_defn = false;
                LinkUseToDef(opn, (JSDefinition *) pn, tc);
            }
        }
        break;

      case PN_NAMESET:
        pn->pn_names = opn->pn_names;
        NULLCHECK(pn->pn_tree = CloneParseTree(opn->pn_tree, tc));
        break;

      case PN_NULLARY:
        // Even PN_NULLARY may have data (apair for E4X -- what a botch).
        pn->pn_u = opn->pn_u;
        break;

#undef NULLCHECK
    }
    return pn;
}

#endif /* JS_HAS_DESTRUCTURING */

extern const char js_with_statement_str[];

static JSParseNode *
ContainsStmt(JSParseNode *pn, JSTokenType tt)
{
    JSParseNode *pn2, *pnt;

    if (!pn)
        return NULL;
    if (PN_TYPE(pn) == tt)
        return pn;
    switch (pn->pn_arity) {
      case PN_LIST:
        for (pn2 = pn->pn_head; pn2; pn2 = pn2->pn_next) {
            pnt = ContainsStmt(pn2, tt);
            if (pnt)
                return pnt;
        }
        break;
      case PN_TERNARY:
        pnt = ContainsStmt(pn->pn_kid1, tt);
        if (pnt)
            return pnt;
        pnt = ContainsStmt(pn->pn_kid2, tt);
        if (pnt)
            return pnt;
        return ContainsStmt(pn->pn_kid3, tt);
      case PN_BINARY:
        /*
         * Limit recursion if pn is a binary expression, which can't contain a
         * var statement.
         */
        if (pn->pn_op != JSOP_NOP)
            return NULL;
        pnt = ContainsStmt(pn->pn_left, tt);
        if (pnt)
            return pnt;
        return ContainsStmt(pn->pn_right, tt);
      case PN_UNARY:
        if (pn->pn_op != JSOP_NOP)
            return NULL;
        return ContainsStmt(pn->pn_kid, tt);
      case PN_NAME:
        return ContainsStmt(pn->maybeExpr(), tt);
      case PN_NAMESET:
        return ContainsStmt(pn->pn_tree, tt);
      default:;
    }
    return NULL;
}

static JSParseNode *
ReturnOrYield(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc,
              JSParser operandParser)
{
    JSTokenType tt, tt2;
    JSParseNode *pn, *pn2;

    tt = CURRENT_TOKEN(ts).type;
    if (tt == TOK_RETURN && !(tc->flags & TCF_IN_FUNCTION)) {
        js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                    JSMSG_BAD_RETURN_OR_YIELD, js_return_str);
        return NULL;
    }

    pn = NewParseNode(PN_UNARY, tc);
    if (!pn)
        return NULL;

#if JS_HAS_GENERATORS
    if (tt == TOK_YIELD)
        tc->flags |= TCF_FUN_IS_GENERATOR;
#endif

    /* This is ugly, but we don't want to require a semicolon. */
    ts->flags |= TSF_OPERAND;
    tt2 = js_PeekTokenSameLine(cx, ts);
    ts->flags &= ~TSF_OPERAND;
    if (tt2 == TOK_ERROR)
        return NULL;

    if (tt2 != TOK_EOF && tt2 != TOK_EOL && tt2 != TOK_SEMI && tt2 != TOK_RC
#if JS_HAS_GENERATORS
        && (tt != TOK_YIELD ||
            (tt2 != tt && tt2 != TOK_RB && tt2 != TOK_RP &&
             tt2 != TOK_COLON && tt2 != TOK_COMMA))
#endif
        ) {
        pn2 = operandParser(cx, ts, tc);
        if (!pn2)
            return NULL;
#if JS_HAS_GENERATORS
        if (tt == TOK_RETURN)
#endif
            tc->flags |= TCF_RETURN_EXPR;
        pn->pn_pos.end = pn2->pn_pos.end;
        pn->pn_kid = pn2;
    } else {
#if JS_HAS_GENERATORS
        if (tt == TOK_RETURN)
#endif
            tc->flags |= TCF_RETURN_VOID;
    }

    if ((~tc->flags & (TCF_RETURN_EXPR | TCF_FUN_IS_GENERATOR)) == 0) {
        /* As in Python (see PEP-255), disallow return v; in generators. */
        ReportBadReturn(cx, tc, JSREPORT_ERROR,
                        JSMSG_BAD_GENERATOR_RETURN,
                        JSMSG_BAD_ANON_GENERATOR_RETURN);
        return NULL;
    }

    if (JS_HAS_STRICT_OPTION(cx) &&
        (~tc->flags & (TCF_RETURN_EXPR | TCF_RETURN_VOID)) == 0 &&
        !ReportBadReturn(cx, tc, JSREPORT_WARNING | JSREPORT_STRICT,
                         JSMSG_NO_RETURN_VALUE,
                         JSMSG_ANON_NO_RETURN_VALUE)) {
        return NULL;
    }

    return pn;
}

static JSParseNode *
PushLexicalScope(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc,
                 JSStmtInfo *stmt)
{
    JSParseNode *pn;
    JSObject *obj;
    JSObjectBox *blockbox;

    pn = NewParseNode(PN_NAME, tc);
    if (!pn)
        return NULL;

    obj = js_NewBlockObject(cx);
    if (!obj)
        return NULL;

    blockbox = tc->compiler->newObjectBox(obj);
    if (!blockbox)
        return NULL;

    js_PushBlockScope(tc, stmt, obj, -1);
    pn->pn_type = TOK_LEXICALSCOPE;
    pn->pn_op = JSOP_LEAVEBLOCK;
    pn->pn_objbox = blockbox;
    pn->pn_cookie = FREE_UPVAR_COOKIE;
    pn->pn_dflags = 0;
    if (!GenerateBlockId(tc, stmt->blockid))
        return NULL;
    pn->pn_blockid = stmt->blockid;
    return pn;
}

#if JS_HAS_BLOCK_SCOPE

static JSParseNode *
LetBlock(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc, JSBool statement)
{
    JSParseNode *pn, *pnblock, *pnlet;
    JSStmtInfo stmtInfo;

    JS_ASSERT(CURRENT_TOKEN(ts).type == TOK_LET);

    /* Create the let binary node. */
    pnlet = NewParseNode(PN_BINARY, tc);
    if (!pnlet)
        return NULL;

    MUST_MATCH_TOKEN(TOK_LP, JSMSG_PAREN_BEFORE_LET);

    /* This is a let block or expression of the form: let (a, b, c) .... */
    pnblock = PushLexicalScope(cx, ts, tc, &stmtInfo);
    if (!pnblock)
        return NULL;
    pn = pnblock;
    pn->pn_expr = pnlet;

    pnlet->pn_left = Variables(cx, ts, tc, true);
    if (!pnlet->pn_left)
        return NULL;
    pnlet->pn_left->pn_xflags = PNX_POPVAR;

    MUST_MATCH_TOKEN(TOK_RP, JSMSG_PAREN_AFTER_LET);

    ts->flags |= TSF_OPERAND;
    if (statement && !js_MatchToken(cx, ts, TOK_LC)) {
        /*
         * If this is really an expression in let statement guise, then we
         * need to wrap the TOK_LET node in a TOK_SEMI node so that we pop
         * the return value of the expression.
         */
        pn = NewParseNode(PN_UNARY, tc);
        if (!pn)
            return NULL;
        pn->pn_type = TOK_SEMI;
        pn->pn_num = -1;
        pn->pn_kid = pnblock;

        statement = JS_FALSE;
    }
    ts->flags &= ~TSF_OPERAND;

    if (statement) {
        pnlet->pn_right = Statements(cx, ts, tc);
        if (!pnlet->pn_right)
            return NULL;
        MUST_MATCH_TOKEN(TOK_RC, JSMSG_CURLY_AFTER_LET);
    } else {
        /*
         * Change pnblock's opcode to the variant that propagates the last
         * result down after popping the block, and clear statement.
         */
        pnblock->pn_op = JSOP_LEAVEBLOCKEXPR;
        pnlet->pn_right = AssignExpr(cx, ts, tc);
        if (!pnlet->pn_right)
            return NULL;
    }

    PopStatement(tc);
    return pn;
}

#endif /* JS_HAS_BLOCK_SCOPE */

static bool
PushBlocklikeStatement(JSStmtInfo *stmt, JSStmtType type, JSTreeContext *tc)
{
    js_PushStatement(tc, stmt, type, -1);
    return GenerateBlockId(tc, stmt->blockid);
}

static JSParseNode *
NewBindingNode(JSAtom *atom, JSTreeContext *tc, bool let = false)
{
    JSParseNode *pn = NULL;

    JSAtomListElement *ale = tc->decls.lookup(atom);
    if (ale) {
        pn = ALE_DEFN(ale);
        JS_ASSERT(!pn->isPlaceholder());
    } else {
        ale = tc->lexdeps.lookup(atom);
        if (ale) {
            pn = ALE_DEFN(ale);
            JS_ASSERT(pn->isPlaceholder());
        }
    }

    if (pn) {
        JS_ASSERT(pn->pn_defn);

        /*
         * A let binding at top level becomes a var before we get here, so if
         * pn and tc have the same blockid then that id must not be the bodyid.
         * If pn is a forward placeholder definition from the same or a higher
         * block then we claim it.
         */
        JS_ASSERT_IF(let && pn->pn_blockid == tc->blockid(),
                     pn->pn_blockid != tc->bodyid);

        if (pn->isPlaceholder() && pn->pn_blockid >= (let ? tc->blockid() : tc->bodyid)) {
            if (let)
                pn->pn_blockid = tc->blockid();

            tc->lexdeps.remove(tc->compiler, atom);
            return pn;
        }
    }

    /* Make a new node for this declarator name (or destructuring pattern). */
    pn = NewNameNode(tc->compiler->context, atom, tc);
    if (!pn)
        return NULL;
    return pn;
}

#if JS_HAS_BLOCK_SCOPE
static bool
RebindLets(JSParseNode *pn, JSTreeContext *tc)
{
    if (!pn)
        return true;

    switch (pn->pn_arity) {
      case PN_LIST:
        for (JSParseNode *pn2 = pn->pn_head; pn2; pn2 = pn2->pn_next)
            RebindLets(pn2, tc);
        break;

      case PN_TERNARY:
        RebindLets(pn->pn_kid1, tc);
        RebindLets(pn->pn_kid2, tc);
        RebindLets(pn->pn_kid3, tc);
        break;

      case PN_BINARY:
        RebindLets(pn->pn_left, tc);
        RebindLets(pn->pn_right, tc);
        break;

      case PN_UNARY:
        RebindLets(pn->pn_kid, tc);
        break;

      case PN_FUNC:
        RebindLets(pn->pn_body, tc);
        break;

      case PN_NAME:
        RebindLets(pn->maybeExpr(), tc);

        if (pn->pn_defn) {
            JS_ASSERT(pn->pn_blockid > tc->topStmt->blockid);
        } else if (pn->pn_used) {
            if (pn->pn_lexdef->pn_blockid == tc->topStmt->blockid) {
                ForgetUse(pn);

                JSAtomListElement *ale = tc->decls.lookup(pn->pn_atom);
                if (ale) {
                    while ((ale = ALE_NEXT(ale)) != NULL) {
                        if (ALE_ATOM(ale) == pn->pn_atom) {
                            LinkUseToDef(pn, ALE_DEFN(ale), tc);
                            return true;
                        }
                    }
                }

                ale = tc->lexdeps.lookup(pn->pn_atom);
                if (!ale) {
                    ale = MakePlaceholder(pn, tc);
                    if (!ale)
                        return NULL;

                    JSDefinition *dn = ALE_DEFN(ale);
                    dn->pn_type = TOK_NAME;
                    dn->pn_op = JSOP_NOP;
                }
                LinkUseToDef(pn, ALE_DEFN(ale), tc);
            }
        }
        break;

      case PN_NAMESET:
        RebindLets(pn->pn_tree, tc);
        break;
    }

    return true;
}
#endif /* JS_HAS_BLOCK_SCOPE */

static JSParseNode *
Statement(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSTokenType tt;
    JSParseNode *pn, *pn1, *pn2, *pn3, *pn4;
    JSStmtInfo stmtInfo, *stmt, *stmt2;
    JSAtom *label;

    JS_CHECK_RECURSION(cx, return NULL);

    ts->flags |= TSF_OPERAND;
    tt = js_GetToken(cx, ts);
    ts->flags &= ~TSF_OPERAND;

#if JS_HAS_GETTER_SETTER
    if (tt == TOK_NAME) {
        tt = CheckGetterOrSetter(cx, ts, TOK_FUNCTION);
        if (tt == TOK_ERROR)
            return NULL;
    }
#endif

    switch (tt) {
      case TOK_FUNCTION:
#if JS_HAS_XML_SUPPORT
        ts->flags |= TSF_KEYWORD_IS_NAME;
        tt = js_PeekToken(cx, ts);
        ts->flags &= ~TSF_KEYWORD_IS_NAME;
        if (tt == TOK_DBLCOLON)
            goto expression;
#endif
        return FunctionStmt(cx, ts, tc);

      case TOK_IF:
        /* An IF node has three kids: condition, then, and optional else. */
        pn = NewParseNode(PN_TERNARY, tc);
        if (!pn)
            return NULL;
        pn1 = Condition(cx, ts, tc);
        if (!pn1)
            return NULL;
        js_PushStatement(tc, &stmtInfo, STMT_IF, -1);
        pn2 = Statement(cx, ts, tc);
        if (!pn2)
            return NULL;
        ts->flags |= TSF_OPERAND;
        if (js_MatchToken(cx, ts, TOK_ELSE)) {
            ts->flags &= ~TSF_OPERAND;
            stmtInfo.type = STMT_ELSE;
            pn3 = Statement(cx, ts, tc);
            if (!pn3)
                return NULL;
            pn->pn_pos.end = pn3->pn_pos.end;
        } else {
            ts->flags &= ~TSF_OPERAND;
            pn3 = NULL;
            pn->pn_pos.end = pn2->pn_pos.end;
        }
        PopStatement(tc);
        pn->pn_kid1 = pn1;
        pn->pn_kid2 = pn2;
        pn->pn_kid3 = pn3;
        return pn;

      case TOK_SWITCH:
      {
        JSParseNode *pn5, *saveBlock;
        JSBool seenDefault = JS_FALSE;

        pn = NewParseNode(PN_BINARY, tc);
        if (!pn)
            return NULL;
        MUST_MATCH_TOKEN(TOK_LP, JSMSG_PAREN_BEFORE_SWITCH);

        /* pn1 points to the switch's discriminant. */
        pn1 = ParenExpr(cx, ts, tc, NULL, NULL);
        if (!pn1)
            return NULL;

        MUST_MATCH_TOKEN(TOK_RP, JSMSG_PAREN_AFTER_SWITCH);
        MUST_MATCH_TOKEN(TOK_LC, JSMSG_CURLY_BEFORE_SWITCH);

        /*
         * NB: we must push stmtInfo before calling GenerateBlockIdForStmtNode
         * because that function states tc->topStmt->blockid.
         */
        js_PushStatement(tc, &stmtInfo, STMT_SWITCH, -1);

        /* pn2 is a list of case nodes. The default case has pn_left == NULL */
        pn2 = NewParseNode(PN_LIST, tc);
        if (!pn2)
            return NULL;
        pn2->makeEmpty();
        if (!GenerateBlockIdForStmtNode(pn2, tc))
            return NULL;
        saveBlock = tc->blockNode;
        tc->blockNode = pn2;

        while ((tt = js_GetToken(cx, ts)) != TOK_RC) {
            switch (tt) {
              case TOK_DEFAULT:
                if (seenDefault) {
                    js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                                JSMSG_TOO_MANY_DEFAULTS);
                    return NULL;
                }
                seenDefault = JS_TRUE;
                /* FALL THROUGH */

              case TOK_CASE:
                pn3 = NewParseNode(PN_BINARY, tc);
                if (!pn3)
                    return NULL;
                if (tt == TOK_CASE) {
                    pn3->pn_left = Expr(cx, ts, tc);
                    if (!pn3->pn_left)
                        return NULL;
                }
                pn2->append(pn3);
                if (pn2->pn_count == JS_BIT(16)) {
                    js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                                JSMSG_TOO_MANY_CASES);
                    return NULL;
                }
                break;

              case TOK_ERROR:
                return NULL;

              default:
                js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                            JSMSG_BAD_SWITCH);
                return NULL;
            }
            MUST_MATCH_TOKEN(TOK_COLON, JSMSG_COLON_AFTER_CASE);

            pn4 = NewParseNode(PN_LIST, tc);
            if (!pn4)
                return NULL;
            pn4->pn_type = TOK_LC;
            pn4->makeEmpty();
            ts->flags |= TSF_OPERAND;
            while ((tt = js_PeekToken(cx, ts)) != TOK_RC &&
                   tt != TOK_CASE && tt != TOK_DEFAULT) {
                ts->flags &= ~TSF_OPERAND;
                if (tt == TOK_ERROR)
                    return NULL;
                pn5 = Statement(cx, ts, tc);
                if (!pn5)
                    return NULL;
                pn4->pn_pos.end = pn5->pn_pos.end;
                pn4->append(pn5);
                ts->flags |= TSF_OPERAND;
            }
            ts->flags &= ~TSF_OPERAND;

            /* Fix the PN_LIST so it doesn't begin at the TOK_COLON. */
            if (pn4->pn_head)
                pn4->pn_pos.begin = pn4->pn_head->pn_pos.begin;
            pn3->pn_pos.end = pn4->pn_pos.end;
            pn3->pn_right = pn4;
        }

        /*
         * Handle the case where there was a let declaration in any case in
         * the switch body, but not within an inner block.  If it replaced
         * tc->blockNode with a new block node then we must refresh pn2 and
         * then restore tc->blockNode.
         */
        if (tc->blockNode != pn2)
            pn2 = tc->blockNode;
        tc->blockNode = saveBlock;
        PopStatement(tc);

        pn->pn_pos.end = pn2->pn_pos.end = CURRENT_TOKEN(ts).pos.end;
        pn->pn_left = pn1;
        pn->pn_right = pn2;
        return pn;
      }

      case TOK_WHILE:
        pn = NewParseNode(PN_BINARY, tc);
        if (!pn)
            return NULL;
        js_PushStatement(tc, &stmtInfo, STMT_WHILE_LOOP, -1);
        pn2 = Condition(cx, ts, tc);
        if (!pn2)
            return NULL;
        pn->pn_left = pn2;
        pn2 = Statement(cx, ts, tc);
        if (!pn2)
            return NULL;
        PopStatement(tc);
        pn->pn_pos.end = pn2->pn_pos.end;
        pn->pn_right = pn2;
        return pn;

      case TOK_DO:
        pn = NewParseNode(PN_BINARY, tc);
        if (!pn)
            return NULL;
        js_PushStatement(tc, &stmtInfo, STMT_DO_LOOP, -1);
        pn2 = Statement(cx, ts, tc);
        if (!pn2)
            return NULL;
        pn->pn_left = pn2;
        MUST_MATCH_TOKEN(TOK_WHILE, JSMSG_WHILE_AFTER_DO);
        pn2 = Condition(cx, ts, tc);
        if (!pn2)
            return NULL;
        PopStatement(tc);
        pn->pn_pos.end = pn2->pn_pos.end;
        pn->pn_right = pn2;
        if (JSVERSION_NUMBER(cx) != JSVERSION_ECMA_3) {
            /*
             * All legacy and extended versions must do automatic semicolon
             * insertion after do-while.  See the testcase and discussion in
             * http://bugzilla.mozilla.org/show_bug.cgi?id=238945.
             */
            (void) js_MatchToken(cx, ts, TOK_SEMI);
            return pn;
        }
        break;

      case TOK_FOR:
      {
        JSParseNode *pnseq = NULL;
#if JS_HAS_BLOCK_SCOPE
        JSParseNode *pnlet = NULL;
        JSStmtInfo blockInfo;
#endif

        /* A FOR node is binary, left is loop control and right is the body. */
        pn = NewParseNode(PN_BINARY, tc);
        if (!pn)
            return NULL;
        js_PushStatement(tc, &stmtInfo, STMT_FOR_LOOP, -1);

        pn->pn_op = JSOP_ITER;
        pn->pn_iflags = 0;
        if (js_MatchToken(cx, ts, TOK_NAME)) {
            if (CURRENT_TOKEN(ts).t_atom == cx->runtime->atomState.eachAtom)
                pn->pn_iflags = JSITER_FOREACH;
            else
                js_UngetToken(ts);
        }

        MUST_MATCH_TOKEN(TOK_LP, JSMSG_PAREN_AFTER_FOR);
        ts->flags |= TSF_OPERAND;
        tt = js_PeekToken(cx, ts);
        ts->flags &= ~TSF_OPERAND;

#if JS_HAS_BLOCK_SCOPE
        bool let = false;
#endif

        if (tt == TOK_SEMI) {
            if (pn->pn_iflags & JSITER_FOREACH)
                goto bad_for_each;

            /* No initializer -- set first kid of left sub-node to null. */
            pn1 = NULL;
        } else {
            /*
             * Set pn1 to a var list or an initializing expression.
             *
             * Set the TCF_IN_FOR_INIT flag during parsing of the first clause
             * of the for statement.  This flag will be used by the RelExpr
             * production; if it is set, then the 'in' keyword will not be
             * recognized as an operator, leaving it available to be parsed as
             * part of a for/in loop.
             *
             * A side effect of this restriction is that (unparenthesized)
             * expressions involving an 'in' operator are illegal in the init
             * clause of an ordinary for loop.
             */
            tc->flags |= TCF_IN_FOR_INIT;
            if (tt == TOK_VAR) {
                (void) js_GetToken(cx, ts);
                pn1 = Variables(cx, ts, tc, false);
#if JS_HAS_BLOCK_SCOPE
            } else if (tt == TOK_LET) {
                let = true;
                (void) js_GetToken(cx, ts);
                if (js_PeekToken(cx, ts) == TOK_LP) {
                    pn1 = LetBlock(cx, ts, tc, JS_FALSE);
                    tt = TOK_LEXICALSCOPE;
                } else {
                    pnlet = PushLexicalScope(cx, ts, tc, &blockInfo);
                    if (!pnlet)
                        return NULL;
                    blockInfo.flags |= SIF_FOR_BLOCK;
                    pn1 = Variables(cx, ts, tc, false);
                }
#endif
            } else {
                pn1 = Expr(cx, ts, tc);
            }
            tc->flags &= ~TCF_IN_FOR_INIT;
            if (!pn1)
                return NULL;
        }

        /*
         * We can be sure that it's a for/in loop if there's still an 'in'
         * keyword here, even if JavaScript recognizes 'in' as an operator,
         * as we've excluded 'in' from being parsed in RelExpr by setting
         * the TCF_IN_FOR_INIT flag in our JSTreeContext.
         */
        if (pn1 && js_MatchToken(cx, ts, TOK_IN)) {
            pn->pn_iflags |= JSITER_ENUMERATE;
            stmtInfo.type = STMT_FOR_IN_LOOP;

            /* Check that the left side of the 'in' is valid. */
            JS_ASSERT(!TOKEN_TYPE_IS_DECL(tt) || PN_TYPE(pn1) == tt);
            if (TOKEN_TYPE_IS_DECL(tt)
                ? (pn1->pn_count > 1 || pn1->pn_op == JSOP_DEFCONST
#if JS_HAS_DESTRUCTURING
                   || (JSVERSION_NUMBER(cx) == JSVERSION_1_7 &&
                       pn->pn_op == JSOP_ITER &&
                       !(pn->pn_iflags & JSITER_FOREACH) &&
                       (pn1->pn_head->pn_type == TOK_RC ||
                        (pn1->pn_head->pn_type == TOK_RB &&
                         pn1->pn_head->pn_count != 2) ||
                        (pn1->pn_head->pn_type == TOK_ASSIGN &&
                         (pn1->pn_head->pn_left->pn_type != TOK_RB ||
                          pn1->pn_head->pn_left->pn_count != 2))))
#endif
                  )
                : (pn1->pn_type != TOK_NAME &&
                   pn1->pn_type != TOK_DOT &&
#if JS_HAS_DESTRUCTURING
                   ((JSVERSION_NUMBER(cx) == JSVERSION_1_7 &&
                     pn->pn_op == JSOP_ITER &&
                     !(pn->pn_iflags & JSITER_FOREACH))
                    ? (pn1->pn_type != TOK_RB || pn1->pn_count != 2)
                    : (pn1->pn_type != TOK_RB && pn1->pn_type != TOK_RC)) &&
#endif
                   pn1->pn_type != TOK_LP &&
#if JS_HAS_XML_SUPPORT
                   (pn1->pn_type != TOK_UNARYOP ||
                    pn1->pn_op != JSOP_XMLNAME) &&
#endif
                   pn1->pn_type != TOK_LB)) {
                js_ReportCompileErrorNumber(cx, ts, pn1, JSREPORT_ERROR,
                                            JSMSG_BAD_FOR_LEFTSIDE);
                return NULL;
            }

            /* pn2 points to the name or destructuring pattern on in's left. */
            pn2 = NULL;
            uintN dflag = PND_ASSIGNED;

            if (TOKEN_TYPE_IS_DECL(tt)) {
                /* Tell js_EmitTree(TOK_VAR) that pn1 is part of a for/in. */
                pn1->pn_xflags |= PNX_FORINVAR;

                /*
                 * Rewrite 'for (<decl> x = i in o)' where <decl> is 'let',
                 * 'var', or 'const' to hoist the initializer or the entire
                 * decl out of the loop head. TOK_VAR is the type for both
                 * 'var' and 'const'.
                 */
                pn2 = pn1->pn_head;
                if ((pn2->pn_type == TOK_NAME && pn2->maybeExpr())
#if JS_HAS_DESTRUCTURING
                    || pn2->pn_type == TOK_ASSIGN
#endif
                    ) {
                    pnseq = NewParseNode(PN_LIST, tc);
                    if (!pnseq)
                        return NULL;
                    pnseq->pn_type = TOK_SEQ;
                    pnseq->pn_pos.begin = pn->pn_pos.begin;

#if JS_HAS_BLOCK_SCOPE
                    if (tt == TOK_LET) {
                        /*
                         * Hoist just the 'i' from 'for (let x = i in o)' to
                         * before the loop, glued together via pnseq.
                         */
                        pn3 = NewParseNode(PN_UNARY, tc);
                        if (!pn3)
                            return NULL;
                        pn3->pn_type = TOK_SEMI;
                        pn3->pn_op = JSOP_NOP;
#if JS_HAS_DESTRUCTURING
                        if (pn2->pn_type == TOK_ASSIGN) {
                            pn4 = pn2->pn_right;
                            pn2 = pn1->pn_head = pn2->pn_left;
                        } else
#endif
                        {
                            pn4 = pn2->pn_expr;
                            pn2->pn_expr = NULL;
                        }
                        if (!RebindLets(pn4, tc))
                            return NULL;
                        pn3->pn_pos = pn4->pn_pos;
                        pn3->pn_kid = pn4;
                        pnseq->initList(pn3);
                    } else
#endif /* JS_HAS_BLOCK_SCOPE */
                    {
                        dflag = PND_INITIALIZED;

                        /*
                         * All of 'var x = i' is hoisted above 'for (x in o)',
                         * so clear PNX_FORINVAR.
                         *
                         * Request JSOP_POP here since the var is for a simple
                         * name (it is not a destructuring binding's left-hand
                         * side) and it has an initializer.
                         */
                        pn1->pn_xflags &= ~PNX_FORINVAR;
                        pn1->pn_xflags |= PNX_POPVAR;
                        pnseq->initList(pn1);

#if JS_HAS_DESTRUCTURING
                        if (pn2->pn_type == TOK_ASSIGN) {
                            pn1 = CloneParseTree(pn2->pn_left, tc);
                            if (!pn1)
                                return NULL;
                        } else
#endif
                        {
                            JS_ASSERT(pn2->pn_type == TOK_NAME);
                            pn1 = NewNameNode(cx, pn2->pn_atom, tc);
                            if (!pn1)
                                return NULL;
                            pn1->pn_type = TOK_NAME;
                            pn1->pn_op = JSOP_NAME;
                            pn1->pn_pos = pn2->pn_pos;
                            if (pn2->pn_defn)
                                LinkUseToDef(pn1, (JSDefinition *) pn2, tc);
                        }
                        pn2 = pn1;
                    }
                }
            }

            if (!pn2) {
                pn2 = pn1;
                if (pn2->pn_type == TOK_LP &&
                    !MakeSetCall(cx, pn2, tc, JSMSG_BAD_LEFTSIDE_OF_ASS)) {
                    return NULL;
                }
#if JS_HAS_XML_SUPPORT
                if (pn2->pn_type == TOK_UNARYOP)
                    pn2->pn_op = JSOP_BINDXMLNAME;
#endif
            }

            switch (pn2->pn_type) {
              case TOK_NAME:
                /* Beware 'for (arguments in ...)' with or without a 'var'. */
                NoteLValue(cx, pn2, tc, dflag);
                break;

#if JS_HAS_DESTRUCTURING
              case TOK_ASSIGN:
                pn2 = pn2->pn_left;
                JS_ASSERT(pn2->pn_type == TOK_RB || pn2->pn_type == TOK_RC);
                /* FALL THROUGH */
              case TOK_RB:
              case TOK_RC:
                /* Check for valid lvalues in var-less destructuring for-in. */
                if (pn1 == pn2 && !CheckDestructuring(cx, NULL, pn2, NULL, tc))
                    return NULL;

                if (JSVERSION_NUMBER(cx) == JSVERSION_1_7) {
                    /*
                     * Destructuring for-in requires [key, value] enumeration
                     * in JS1.7.
                     */
                    JS_ASSERT(pn->pn_op == JSOP_ITER);
                    if (!(pn->pn_iflags & JSITER_FOREACH))
                        pn->pn_iflags |= JSITER_FOREACH | JSITER_KEYVALUE;
                }
                break;
#endif

              default:;
            }

            /*
             * Parse the object expression as the right operand of 'in', first
             * removing the top statement from the statement-stack if this is a
             * 'for (let x in y)' loop.
             */
#if JS_HAS_BLOCK_SCOPE
            JSStmtInfo *save = tc->topStmt;
            if (let)
                tc->topStmt = save->down;
#endif
            pn2 = Expr(cx, ts, tc);
#if JS_HAS_BLOCK_SCOPE
            if (let)
                tc->topStmt = save;
#endif

            pn2 = NewBinary(TOK_IN, JSOP_NOP, pn1, pn2, tc);
            if (!pn2)
                return NULL;
            pn->pn_left = pn2;
        } else {
            if (pn->pn_iflags & JSITER_FOREACH)
                goto bad_for_each;
            pn->pn_op = JSOP_NOP;

            /* Parse the loop condition or null into pn2. */
            MUST_MATCH_TOKEN(TOK_SEMI, JSMSG_SEMI_AFTER_FOR_INIT);
            ts->flags |= TSF_OPERAND;
            tt = js_PeekToken(cx, ts);
            ts->flags &= ~TSF_OPERAND;
            if (tt == TOK_SEMI) {
                pn2 = NULL;
            } else {
                pn2 = Expr(cx, ts, tc);
                if (!pn2)
                    return NULL;
            }

            /* Parse the update expression or null into pn3. */
            MUST_MATCH_TOKEN(TOK_SEMI, JSMSG_SEMI_AFTER_FOR_COND);
            ts->flags |= TSF_OPERAND;
            tt = js_PeekToken(cx, ts);
            ts->flags &= ~TSF_OPERAND;
            if (tt == TOK_RP) {
                pn3 = NULL;
            } else {
                pn3 = Expr(cx, ts, tc);
                if (!pn3)
                    return NULL;
            }

            /* Build the FORHEAD node to use as the left kid of pn. */
            pn4 = NewParseNode(PN_TERNARY, tc);
            if (!pn4)
                return NULL;
            pn4->pn_type = TOK_FORHEAD;
            pn4->pn_op = JSOP_NOP;
            pn4->pn_kid1 = pn1;
            pn4->pn_kid2 = pn2;
            pn4->pn_kid3 = pn3;
            pn->pn_left = pn4;
        }

        MUST_MATCH_TOKEN(TOK_RP, JSMSG_PAREN_AFTER_FOR_CTRL);

        /* Parse the loop body into pn->pn_right. */
        pn2 = Statement(cx, ts, tc);
        if (!pn2)
            return NULL;
        pn->pn_right = pn2;

        /* Record the absolute line number for source note emission. */
        pn->pn_pos.end = pn2->pn_pos.end;

#if JS_HAS_BLOCK_SCOPE
        if (pnlet) {
            PopStatement(tc);
            pnlet->pn_expr = pn;
            pn = pnlet;
        }
#endif
        if (pnseq) {
            pnseq->pn_pos.end = pn->pn_pos.end;
            pnseq->append(pn);
            pn = pnseq;
        }
        PopStatement(tc);
        return pn;

      bad_for_each:
        js_ReportCompileErrorNumber(cx, ts, pn, JSREPORT_ERROR,
                                    JSMSG_BAD_FOR_EACH_LOOP);
        return NULL;
      }

      case TOK_TRY: {
        JSParseNode *catchList, *lastCatch;

        /*
         * try nodes are ternary.
         * kid1 is the try Statement
         * kid2 is the catch node list or null
         * kid3 is the finally Statement
         *
         * catch nodes are ternary.
         * kid1 is the lvalue (TOK_NAME, TOK_LB, or TOK_LC)
         * kid2 is the catch guard or null if no guard
         * kid3 is the catch block
         *
         * catch lvalue nodes are either:
         *   TOK_NAME for a single identifier
         *   TOK_RB or TOK_RC for a destructuring left-hand side
         *
         * finally nodes are TOK_LC Statement lists.
         */
        pn = NewParseNode(PN_TERNARY, tc);
        if (!pn)
            return NULL;
        pn->pn_op = JSOP_NOP;

        MUST_MATCH_TOKEN(TOK_LC, JSMSG_CURLY_BEFORE_TRY);
        if (!PushBlocklikeStatement(&stmtInfo, STMT_TRY, tc))
            return NULL;
        pn->pn_kid1 = Statements(cx, ts, tc);
        if (!pn->pn_kid1)
            return NULL;
        MUST_MATCH_TOKEN(TOK_RC, JSMSG_CURLY_AFTER_TRY);
        PopStatement(tc);

        catchList = NULL;
        tt = js_GetToken(cx, ts);
        if (tt == TOK_CATCH) {
            catchList = NewParseNode(PN_LIST, tc);
            if (!catchList)
                return NULL;
            catchList->pn_type = TOK_RESERVED;
            catchList->makeEmpty();
            lastCatch = NULL;

            do {
                JSParseNode *pnblock;
                BindData data;

                /* Check for another catch after unconditional catch. */
                if (lastCatch && !lastCatch->pn_kid2) {
                    js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                                JSMSG_CATCH_AFTER_GENERAL);
                    return NULL;
                }

                /*
                 * Create a lexical scope node around the whole catch clause,
                 * including the head.
                 */
                pnblock = PushLexicalScope(cx, ts, tc, &stmtInfo);
                if (!pnblock)
                    return NULL;
                stmtInfo.type = STMT_CATCH;

                /*
                 * Legal catch forms are:
                 *   catch (lhs)
                 *   catch (lhs if <boolean_expression>)
                 * where lhs is a name or a destructuring left-hand side.
                 * (the latter is legal only #ifdef JS_HAS_CATCH_GUARD)
                 */
                pn2 = NewParseNode(PN_TERNARY, tc);
                if (!pn2)
                    return NULL;
                pnblock->pn_expr = pn2;
                MUST_MATCH_TOKEN(TOK_LP, JSMSG_PAREN_BEFORE_CATCH);

                /*
                 * Contrary to ECMA Ed. 3, the catch variable is lexically
                 * scoped, not a property of a new Object instance.  This is
                 * an intentional change that anticipates ECMA Ed. 4.
                 */
                data.pn = NULL;
                data.op = JSOP_NOP;
                data.binder = BindLet;
                data.let.overflow = JSMSG_TOO_MANY_CATCH_VARS;

                tt = js_GetToken(cx, ts);
                switch (tt) {
#if JS_HAS_DESTRUCTURING
                  case TOK_LB:
                  case TOK_LC:
                    pn3 = DestructuringExpr(cx, &data, tc, tt);
                    if (!pn3)
                        return NULL;
                    break;
#endif

                  case TOK_NAME:
                    label = CURRENT_TOKEN(ts).t_atom;
                    pn3 = NewBindingNode(label, tc, true);
                    if (!pn3)
                        return NULL;
                    data.pn = pn3;
                    if (!data.binder(cx, &data, label, tc))
                        return NULL;
                    break;

                  default:
                    js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                                JSMSG_CATCH_IDENTIFIER);
                    return NULL;
                }

                pn2->pn_kid1 = pn3;
#if JS_HAS_CATCH_GUARD
                /*
                 * We use 'catch (x if x === 5)' (not 'catch (x : x === 5)')
                 * to avoid conflicting with the JS2/ECMAv4 type annotation
                 * catchguard syntax.
                 */
                if (js_MatchToken(cx, ts, TOK_IF)) {
                    pn2->pn_kid2 = Expr(cx, ts, tc);
                    if (!pn2->pn_kid2)
                        return NULL;
                }
#endif
                MUST_MATCH_TOKEN(TOK_RP, JSMSG_PAREN_AFTER_CATCH);

                MUST_MATCH_TOKEN(TOK_LC, JSMSG_CURLY_BEFORE_CATCH);
                pn2->pn_kid3 = Statements(cx, ts, tc);
                if (!pn2->pn_kid3)
                    return NULL;
                MUST_MATCH_TOKEN(TOK_RC, JSMSG_CURLY_AFTER_CATCH);
                PopStatement(tc);

                catchList->append(pnblock);
                lastCatch = pn2;
                ts->flags |= TSF_OPERAND;
                tt = js_GetToken(cx, ts);
                ts->flags &= ~TSF_OPERAND;
            } while (tt == TOK_CATCH);
        }
        pn->pn_kid2 = catchList;

        if (tt == TOK_FINALLY) {
            MUST_MATCH_TOKEN(TOK_LC, JSMSG_CURLY_BEFORE_FINALLY);
            if (!PushBlocklikeStatement(&stmtInfo, STMT_FINALLY, tc))
                return NULL;
            pn->pn_kid3 = Statements(cx, ts, tc);
            if (!pn->pn_kid3)
                return NULL;
            MUST_MATCH_TOKEN(TOK_RC, JSMSG_CURLY_AFTER_FINALLY);
            PopStatement(tc);
        } else {
            js_UngetToken(ts);
        }
        if (!catchList && !pn->pn_kid3) {
            js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                        JSMSG_CATCH_OR_FINALLY);
            return NULL;
        }
        return pn;
      }

      case TOK_THROW:
        pn = NewParseNode(PN_UNARY, tc);
        if (!pn)
            return NULL;

        /* ECMA-262 Edition 3 says 'throw [no LineTerminator here] Expr'. */
        ts->flags |= TSF_OPERAND;
        tt = js_PeekTokenSameLine(cx, ts);
        ts->flags &= ~TSF_OPERAND;
        if (tt == TOK_ERROR)
            return NULL;
        if (tt == TOK_EOF || tt == TOK_EOL || tt == TOK_SEMI || tt == TOK_RC) {
            js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                        JSMSG_SYNTAX_ERROR);
            return NULL;
        }

        pn2 = Expr(cx, ts, tc);
        if (!pn2)
            return NULL;
        pn->pn_pos.end = pn2->pn_pos.end;
        pn->pn_op = JSOP_THROW;
        pn->pn_kid = pn2;
        break;

      /* TOK_CATCH and TOK_FINALLY are both handled in the TOK_TRY case */
      case TOK_CATCH:
        js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                    JSMSG_CATCH_WITHOUT_TRY);
        return NULL;

      case TOK_FINALLY:
        js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                    JSMSG_FINALLY_WITHOUT_TRY);
        return NULL;

      case TOK_BREAK:
        pn = NewParseNode(PN_NULLARY, tc);
        if (!pn)
            return NULL;
        if (!MatchLabel(cx, ts, pn))
            return NULL;
        stmt = tc->topStmt;
        label = pn->pn_atom;
        if (label) {
            for (; ; stmt = stmt->down) {
                if (!stmt) {
                    js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                                JSMSG_LABEL_NOT_FOUND);
                    return NULL;
                }
                if (stmt->type == STMT_LABEL && stmt->label == label)
                    break;
            }
        } else {
            for (; ; stmt = stmt->down) {
                if (!stmt) {
                    js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                                JSMSG_TOUGH_BREAK);
                    return NULL;
                }
                if (STMT_IS_LOOP(stmt) || stmt->type == STMT_SWITCH)
                    break;
            }
        }
        if (label)
            pn->pn_pos.end = CURRENT_TOKEN(ts).pos.end;
        break;

      case TOK_CONTINUE:
        pn = NewParseNode(PN_NULLARY, tc);
        if (!pn)
            return NULL;
        if (!MatchLabel(cx, ts, pn))
            return NULL;
        stmt = tc->topStmt;
        label = pn->pn_atom;
        if (label) {
            for (stmt2 = NULL; ; stmt = stmt->down) {
                if (!stmt) {
                    js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                                JSMSG_LABEL_NOT_FOUND);
                    return NULL;
                }
                if (stmt->type == STMT_LABEL) {
                    if (stmt->label == label) {
                        if (!stmt2 || !STMT_IS_LOOP(stmt2)) {
                            js_ReportCompileErrorNumber(cx, ts, NULL,
                                                        JSREPORT_ERROR,
                                                        JSMSG_BAD_CONTINUE);
                            return NULL;
                        }
                        break;
                    }
                } else {
                    stmt2 = stmt;
                }
            }
        } else {
            for (; ; stmt = stmt->down) {
                if (!stmt) {
                    js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                                JSMSG_BAD_CONTINUE);
                    return NULL;
                }
                if (STMT_IS_LOOP(stmt))
                    break;
            }
        }
        if (label)
            pn->pn_pos.end = CURRENT_TOKEN(ts).pos.end;
        break;

      case TOK_WITH:
        pn = NewParseNode(PN_BINARY, tc);
        if (!pn)
            return NULL;
        MUST_MATCH_TOKEN(TOK_LP, JSMSG_PAREN_BEFORE_WITH);
        pn2 = ParenExpr(cx, ts, tc, NULL, NULL);
        if (!pn2)
            return NULL;
        MUST_MATCH_TOKEN(TOK_RP, JSMSG_PAREN_AFTER_WITH);
        pn->pn_left = pn2;

        js_PushStatement(tc, &stmtInfo, STMT_WITH, -1);
        pn2 = Statement(cx, ts, tc);
        if (!pn2)
            return NULL;
        PopStatement(tc);

        pn->pn_pos.end = pn2->pn_pos.end;
        pn->pn_right = pn2;
        tc->flags |= TCF_FUN_HEAVYWEIGHT;
        return pn;

      case TOK_VAR:
        pn = Variables(cx, ts, tc, false);
        if (!pn)
            return NULL;

        /* Tell js_EmitTree to generate a final POP. */
        pn->pn_xflags |= PNX_POPVAR;
        break;

#if JS_HAS_BLOCK_SCOPE
      case TOK_LET:
      {
        JSObject *obj;
        JSObjectBox *blockbox;

        /* Check for a let statement or let expression. */
        if (js_PeekToken(cx, ts) == TOK_LP) {
            pn = LetBlock(cx, ts, tc, JS_TRUE);
            if (!pn || pn->pn_op == JSOP_LEAVEBLOCK)
                return pn;

            /* Let expressions require automatic semicolon insertion. */
            JS_ASSERT(pn->pn_type == TOK_SEMI ||
                      pn->pn_op == JSOP_LEAVEBLOCKEXPR);
            break;
        }

        /*
         * This is a let declaration. We must be directly under a block per
         * the proposed ES4 specs, but not an implicit block created due to
         * 'for (let ...)'. If we pass this error test, make the enclosing
         * JSStmtInfo be our scope. Further let declarations in this block
         * will find this scope statement and use the same block object.
         *
         * If we are the first let declaration in this block (i.e., when the
         * enclosing maybe-scope JSStmtInfo isn't yet a scope statement) then
         * we also need to set tc->blockNode to be our TOK_LEXICALSCOPE.
         */
        stmt = tc->topStmt;
        if (stmt &&
            (!STMT_MAYBE_SCOPE(stmt) || (stmt->flags & SIF_FOR_BLOCK))) {
            js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                        JSMSG_LET_DECL_NOT_IN_BLOCK);
            return NULL;
        }

        if (stmt && (stmt->flags & SIF_SCOPE)) {
            JS_ASSERT(tc->blockChain == stmt->blockObj);
            obj = tc->blockChain;
        } else {
            if (!stmt || (stmt->flags & SIF_BODY_BLOCK)) {
                /*
                 * ES4 specifies that let at top level and at body-block scope
                 * does not shadow var, so convert back to var.
                 */
                CURRENT_TOKEN(ts).type = TOK_VAR;
                CURRENT_TOKEN(ts).t_op = JSOP_DEFVAR;

                pn = Variables(cx, ts, tc, false);
                if (!pn)
                    return NULL;
                pn->pn_xflags |= PNX_POPVAR;
                break;
            }

            /*
             * Some obvious assertions here, but they may help clarify the
             * situation. This stmt is not yet a scope, so it must not be a
             * catch block (catch is a lexical scope by definition).
             */
            JS_ASSERT(!(stmt->flags & SIF_SCOPE));
            JS_ASSERT(stmt != tc->topScopeStmt);
            JS_ASSERT(stmt->type == STMT_BLOCK ||
                      stmt->type == STMT_SWITCH ||
                      stmt->type == STMT_TRY ||
                      stmt->type == STMT_FINALLY);
            JS_ASSERT(!stmt->downScope);

            /* Convert the block statement into a scope statement. */
            JSObject *obj = js_NewBlockObject(tc->compiler->context);
            if (!obj)
                return NULL;

            blockbox = tc->compiler->newObjectBox(obj);
            if (!blockbox)
                return NULL;

            /*
             * Insert stmt on the tc->topScopeStmt/stmtInfo.downScope linked
             * list stack, if it isn't already there.  If it is there, but it
             * lacks the SIF_SCOPE flag, it must be a try, catch, or finally
             * block.
             */
            stmt->flags |= SIF_SCOPE;
            stmt->downScope = tc->topScopeStmt;
            tc->topScopeStmt = stmt;
            JS_SCOPE_DEPTH_METERING(++tc->scopeDepth > tc->maxScopeDepth &&
                                    (tc->maxScopeDepth = tc->scopeDepth));

            STOBJ_SET_PARENT(obj, tc->blockChain);
            tc->blockChain = obj;
            stmt->blockObj = obj;

#ifdef DEBUG
            pn1 = tc->blockNode;
            JS_ASSERT(!pn1 || pn1->pn_type != TOK_LEXICALSCOPE);
#endif

            /* Create a new lexical scope node for these statements. */
            pn1 = NewParseNode(PN_NAME, tc);
            if (!pn1)
                return NULL;

            pn1->pn_type = TOK_LEXICALSCOPE;
            pn1->pn_op = JSOP_LEAVEBLOCK;
            pn1->pn_pos = tc->blockNode->pn_pos;
            pn1->pn_objbox = blockbox;
            pn1->pn_expr = tc->blockNode;
            pn1->pn_blockid = tc->blockNode->pn_blockid;
            tc->blockNode = pn1;
        }

        pn = Variables(cx, ts, tc, false);
        if (!pn)
            return NULL;
        pn->pn_xflags = PNX_POPVAR;
        break;
      }
#endif /* JS_HAS_BLOCK_SCOPE */

      case TOK_RETURN:
        pn = ReturnOrYield(cx, ts, tc, Expr);
        if (!pn)
            return NULL;
        break;

      case TOK_LC:
      {
        uintN oldflags;

        oldflags = tc->flags;
        tc->flags = oldflags & ~TCF_HAS_FUNCTION_STMT;
        if (!PushBlocklikeStatement(&stmtInfo, STMT_BLOCK, tc))
            return NULL;
        pn = Statements(cx, ts, tc);
        if (!pn)
            return NULL;

        MUST_MATCH_TOKEN(TOK_RC, JSMSG_CURLY_IN_COMPOUND);
        PopStatement(tc);

        /*
         * If we contain a function statement and our container is top-level
         * or another block, flag pn to preserve braces when decompiling.
         */
        if ((tc->flags & TCF_HAS_FUNCTION_STMT) &&
            (!tc->topStmt || tc->topStmt->type == STMT_BLOCK)) {
            pn->pn_xflags |= PNX_NEEDBRACES;
        }
        tc->flags = oldflags | (tc->flags & (TCF_FUN_FLAGS | TCF_RETURN_FLAGS));
        return pn;
      }

      case TOK_EOL:
      case TOK_SEMI:
        pn = NewParseNode(PN_UNARY, tc);
        if (!pn)
            return NULL;
        pn->pn_type = TOK_SEMI;
        return pn;

#if JS_HAS_DEBUGGER_KEYWORD
      case TOK_DEBUGGER:
        pn = NewParseNode(PN_NULLARY, tc);
        if (!pn)
            return NULL;
        pn->pn_type = TOK_DEBUGGER;
        tc->flags |= TCF_FUN_HEAVYWEIGHT;
        break;
#endif /* JS_HAS_DEBUGGER_KEYWORD */

#if JS_HAS_XML_SUPPORT
      case TOK_DEFAULT:
        pn = NewParseNode(PN_UNARY, tc);
        if (!pn)
            return NULL;
        if (!js_MatchToken(cx, ts, TOK_NAME) ||
            CURRENT_TOKEN(ts).t_atom != cx->runtime->atomState.xmlAtom ||
            !js_MatchToken(cx, ts, TOK_NAME) ||
            CURRENT_TOKEN(ts).t_atom != cx->runtime->atomState.namespaceAtom ||
            !js_MatchToken(cx, ts, TOK_ASSIGN) ||
            CURRENT_TOKEN(ts).t_op != JSOP_NOP) {
            js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                        JSMSG_BAD_DEFAULT_XML_NAMESPACE);
            return NULL;
        }

        /* Is this an E4X dagger I see before me? */
        tc->flags |= TCF_FUN_HEAVYWEIGHT;
        pn2 = Expr(cx, ts, tc);
        if (!pn2)
            return NULL;
        pn->pn_op = JSOP_DEFXMLNS;
        pn->pn_pos.end = pn2->pn_pos.end;
        pn->pn_kid = pn2;
        break;
#endif

      case TOK_ERROR:
        return NULL;

      default:
#if JS_HAS_XML_SUPPORT
      expression:
#endif
        js_UngetToken(ts);
        pn2 = Expr(cx, ts, tc);
        if (!pn2)
            return NULL;

        if (js_PeekToken(cx, ts) == TOK_COLON) {
            if (pn2->pn_type != TOK_NAME) {
                js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                            JSMSG_BAD_LABEL);
                return NULL;
            }
            label = pn2->pn_atom;
            for (stmt = tc->topStmt; stmt; stmt = stmt->down) {
                if (stmt->type == STMT_LABEL && stmt->label == label) {
                    js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                                JSMSG_DUPLICATE_LABEL);
                    return NULL;
                }
            }
            ForgetUse(pn2);

            (void) js_GetToken(cx, ts);

            /* Push a label struct and parse the statement. */
            js_PushStatement(tc, &stmtInfo, STMT_LABEL, -1);
            stmtInfo.label = label;
            pn = Statement(cx, ts, tc);
            if (!pn)
                return NULL;

            /* Normalize empty statement to empty block for the decompiler. */
            if (pn->pn_type == TOK_SEMI && !pn->pn_kid) {
                pn->pn_type = TOK_LC;
                pn->pn_arity = PN_LIST;
                pn->makeEmpty();
            }

            /* Pop the label, set pn_expr, and return early. */
            PopStatement(tc);
            pn2->pn_type = TOK_COLON;
            pn2->pn_pos.end = pn->pn_pos.end;
            pn2->pn_expr = pn;
            return pn2;
        }

        pn = NewParseNode(PN_UNARY, tc);
        if (!pn)
            return NULL;
        pn->pn_type = TOK_SEMI;
        pn->pn_pos = pn2->pn_pos;
        pn->pn_kid = pn2;

        /*
         * Specialize JSOP_SETPROP into JSOP_SETMETHOD to defer or avoid null
         * closure cloning. Do this here rather than in AssignExpr as only now
         * do we know that the uncloned (unjoined in ES3 terms) function object
         * result of the assignment expression can't escape.
         */
        if (PN_TYPE(pn2) == TOK_ASSIGN && PN_OP(pn2) == JSOP_NOP &&
            PN_OP(pn2->pn_left) == JSOP_SETPROP &&
            PN_OP(pn2->pn_right) == JSOP_LAMBDA &&
            !(pn2->pn_right->pn_funbox->tcflags
              & (TCF_FUN_USES_ARGUMENTS | TCF_FUN_USES_OWN_NAME))) {
            pn2->pn_left->pn_op = JSOP_SETMETHOD;
        }
        break;
    }

    /* Check termination of this primitive statement. */
    return MatchOrInsertSemicolon(cx, ts) ? pn : NULL;
}

static void
NoteArgumentsUse(JSTreeContext *tc)
{
    JS_ASSERT(tc->flags & TCF_IN_FUNCTION);
    tc->flags |= TCF_FUN_USES_ARGUMENTS;
    if (tc->funbox)
        tc->funbox->node->pn_dflags |= PND_FUNARG;
}

static JSParseNode *
Variables(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc, bool inLetHead)
{
    JSTokenType tt;
    bool let;
    JSStmtInfo *scopeStmt;
    BindData data;
    JSParseNode *pn, *pn2;
    JSAtom *atom;

    /*
     * The three options here are:
     * - TOK_LET: We are parsing a let declaration.
     * - TOK_LP: We are parsing the head of a let block.
     * - Otherwise, we're parsing var declarations.
     */
    tt = CURRENT_TOKEN(ts).type;
    let = (tt == TOK_LET || tt == TOK_LP);
    JS_ASSERT(let || tt == TOK_VAR);

#if JS_HAS_BLOCK_SCOPE
    bool popScope = (inLetHead || (let && (tc->flags & TCF_IN_FOR_INIT)));
    JSStmtInfo *save = tc->topStmt, *saveScope = tc->topScopeStmt;
#endif

    /* Make sure that Statement set up the tree context correctly. */
    scopeStmt = tc->topScopeStmt;
    if (let) {
        while (scopeStmt && !(scopeStmt->flags & SIF_SCOPE)) {
            JS_ASSERT(!STMT_MAYBE_SCOPE(scopeStmt));
            scopeStmt = scopeStmt->downScope;
        }
        JS_ASSERT(scopeStmt);
    }

    data.op = let ? JSOP_NOP : CURRENT_TOKEN(ts).t_op;
    pn = NewParseNode(PN_LIST, tc);
    if (!pn)
        return NULL;
    pn->pn_op = data.op;
    pn->makeEmpty();

    /*
     * SpiderMonkey const is really "write once per initialization evaluation"
     * var, whereas let is block scoped. ES-Harmony wants block-scoped const so
     * this code will change soon.
     */
    if (let) {
        JS_ASSERT(tc->blockChain == scopeStmt->blockObj);
        data.binder = BindLet;
        data.let.overflow = JSMSG_TOO_MANY_LOCALS;
    } else {
        data.binder = BindVarOrConst;
    }

    do {
        tt = js_GetToken(cx, ts);
#if JS_HAS_DESTRUCTURING
        if (tt == TOK_LB || tt == TOK_LC) {
            tc->flags |= TCF_DECL_DESTRUCTURING;
            pn2 = PrimaryExpr(cx, ts, tc, tt, JS_FALSE);
            tc->flags &= ~TCF_DECL_DESTRUCTURING;
            if (!pn2)
                return NULL;

            if (!CheckDestructuring(cx, &data, pn2, NULL, tc))
                return NULL;
            if ((tc->flags & TCF_IN_FOR_INIT) &&
                js_PeekToken(cx, ts) == TOK_IN) {
                pn->append(pn2);
                continue;
            }

            MUST_MATCH_TOKEN(TOK_ASSIGN, JSMSG_BAD_DESTRUCT_DECL);
            if (CURRENT_TOKEN(ts).t_op != JSOP_NOP)
                goto bad_var_init;

#if JS_HAS_BLOCK_SCOPE
            if (popScope) {
                tc->topStmt = save->down;
                tc->topScopeStmt = saveScope->downScope;
            }
#endif
            JSParseNode *init = AssignExpr(cx, ts, tc);
#if JS_HAS_BLOCK_SCOPE
            if (popScope) {
                tc->topStmt = save;
                tc->topScopeStmt = saveScope;
            }
#endif

            if (!init || !UndominateInitializers(pn2, init, tc))
                return NULL;

            pn2 = NewBinary(TOK_ASSIGN, JSOP_NOP, pn2, init, tc);
            if (!pn2)
                return NULL;
            pn->append(pn2);
            continue;
        }
#endif /* JS_HAS_DESTRUCTURING */

        if (tt != TOK_NAME) {
            if (tt != TOK_ERROR) {
                js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                            JSMSG_NO_VARIABLE_NAME);
            }
            return NULL;
        }

        atom = CURRENT_TOKEN(ts).t_atom;
        pn2 = NewBindingNode(atom, tc, let);
        if (!pn2)
            return NULL;
        if (data.op == JSOP_DEFCONST)
            pn2->pn_dflags |= PND_CONST;
        data.pn = pn2;
        if (!data.binder(cx, &data, atom, tc))
            return NULL;
        pn->append(pn2);

        if (js_MatchToken(cx, ts, TOK_ASSIGN)) {
            if (CURRENT_TOKEN(ts).t_op != JSOP_NOP)
                goto bad_var_init;

#if JS_HAS_BLOCK_SCOPE
            if (popScope) {
                tc->topStmt = save->down;
                tc->topScopeStmt = saveScope->downScope;
            }
#endif
            JSParseNode *init = AssignExpr(cx, ts, tc);
#if JS_HAS_BLOCK_SCOPE
            if (popScope) {
                tc->topStmt = save;
                tc->topScopeStmt = saveScope;
            }
#endif
            if (!init)
                return NULL;

            if (pn2->pn_used) {
                pn2 = MakeAssignment(pn2, init, tc);
                if (!pn2)
                    return NULL;
            } else {
                pn2->pn_expr = init;
            }

            pn2->pn_op = (PN_OP(pn2) == JSOP_ARGUMENTS)
                         ? JSOP_SETNAME
                         : (pn2->pn_dflags & PND_GVAR)
                         ? JSOP_SETGVAR
                         : (pn2->pn_dflags & PND_BOUND)
                         ? JSOP_SETLOCAL
                         : (data.op == JSOP_DEFCONST)
                         ? JSOP_SETCONST
                         : JSOP_SETNAME;

            NoteLValue(cx, pn2, tc, data.fresh ? PND_INITIALIZED : PND_ASSIGNED);

            /* The declarator's position must include the initializer. */
            pn2->pn_pos.end = init->pn_pos.end;

            if ((tc->flags & TCF_IN_FUNCTION) &&
                atom == cx->runtime->atomState.argumentsAtom) {
                NoteArgumentsUse(tc);
                if (!let)
                    tc->flags |= TCF_FUN_HEAVYWEIGHT;
            }
        }
    } while (js_MatchToken(cx, ts, TOK_COMMA));

    pn->pn_pos.end = pn->last()->pn_pos.end;
    return pn;

bad_var_init:
    js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                JSMSG_BAD_VAR_INIT);
    return NULL;
}

static JSParseNode *
Expr(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSParseNode *pn, *pn2;

    pn = AssignExpr(cx, ts, tc);
    if (pn && js_MatchToken(cx, ts, TOK_COMMA)) {
        pn2 = NewParseNode(PN_LIST, tc);
        if (!pn2)
            return NULL;
        pn2->pn_pos.begin = pn->pn_pos.begin;
        pn2->initList(pn);
        pn = pn2;
        do {
#if JS_HAS_GENERATORS
            pn2 = pn->last();
            if (pn2->pn_type == TOK_YIELD && !pn2->pn_parens) {
                js_ReportCompileErrorNumber(cx, ts, pn2, JSREPORT_ERROR,
                                            JSMSG_BAD_GENERATOR_SYNTAX,
                                            js_yield_str);
                return NULL;
            }
#endif
            pn2 = AssignExpr(cx, ts, tc);
            if (!pn2)
                return NULL;
            pn->append(pn2);
        } while (js_MatchToken(cx, ts, TOK_COMMA));
        pn->pn_pos.end = pn->last()->pn_pos.end;
    }
    return pn;
}

static JSParseNode *
AssignExpr(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSParseNode *pn, *rhs;
    JSTokenType tt;
    JSOp op;

    JS_CHECK_RECURSION(cx, return NULL);

#if JS_HAS_GENERATORS
    ts->flags |= TSF_OPERAND;
    if (js_MatchToken(cx, ts, TOK_YIELD)) {
        ts->flags &= ~TSF_OPERAND;
        return ReturnOrYield(cx, ts, tc, AssignExpr);
    }
    ts->flags &= ~TSF_OPERAND;
#endif

    pn = CondExpr(cx, ts, tc);
    if (!pn)
        return NULL;

    tt = js_GetToken(cx, ts);
#if JS_HAS_GETTER_SETTER
    if (tt == TOK_NAME) {
        tt = CheckGetterOrSetter(cx, ts, TOK_ASSIGN);
        if (tt == TOK_ERROR)
            return NULL;
    }
#endif
    if (tt != TOK_ASSIGN) {
        js_UngetToken(ts);
        return pn;
    }

    op = CURRENT_TOKEN(ts).t_op;
    switch (pn->pn_type) {
      case TOK_NAME:
        pn->pn_op = JSOP_SETNAME;
        NoteLValue(cx, pn, tc);
        break;
      case TOK_DOT:
        pn->pn_op = JSOP_SETPROP;
        break;
      case TOK_LB:
        pn->pn_op = JSOP_SETELEM;
        break;
#if JS_HAS_DESTRUCTURING
      case TOK_RB:
      case TOK_RC:
        if (op != JSOP_NOP) {
            js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                        JSMSG_BAD_DESTRUCT_ASS);
            return NULL;
        }
        rhs = AssignExpr(cx, ts, tc);
        if (!rhs || !CheckDestructuring(cx, NULL, pn, rhs, tc))
            return NULL;
        return NewBinary(TOK_ASSIGN, op, pn, rhs, tc);
#endif
      case TOK_LP:
        if (!MakeSetCall(cx, pn, tc, JSMSG_BAD_LEFTSIDE_OF_ASS))
            return NULL;
        break;
#if JS_HAS_XML_SUPPORT
      case TOK_UNARYOP:
        if (pn->pn_op == JSOP_XMLNAME) {
            pn->pn_op = JSOP_SETXMLNAME;
            break;
        }
        /* FALL THROUGH */
#endif
      default:
        js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                    JSMSG_BAD_LEFTSIDE_OF_ASS);
        return NULL;
    }

    rhs = AssignExpr(cx, ts, tc);
    if (rhs && PN_TYPE(pn) == TOK_NAME && pn->pn_used) {
        JSDefinition *dn = pn->pn_lexdef;

        /*
         * If the definition is not flagged as assigned, we must have imputed
         * the initialized flag to it, to optimize for flat closures. But that
         * optimization uses source coordinates to check dominance relations,
         * so we must extend the end of the definition to cover the right-hand
         * side of this assignment, i.e., the initializer.
         */
        if (!dn->isAssigned()) {
            JS_ASSERT(dn->isInitialized());
            dn->pn_pos.end = rhs->pn_pos.end;
        }
    }

    return NewBinary(TOK_ASSIGN, op, pn, rhs, tc);
}

static JSParseNode *
CondExpr(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSParseNode *pn, *pn1, *pn2, *pn3;
    uintN oldflags;

    pn = OrExpr(cx, ts, tc);
    if (pn && js_MatchToken(cx, ts, TOK_HOOK)) {
        pn1 = pn;
        pn = NewParseNode(PN_TERNARY, tc);
        if (!pn)
            return NULL;
        /*
         * Always accept the 'in' operator in the middle clause of a ternary,
         * where it's unambiguous, even if we might be parsing the init of a
         * for statement.
         */
        oldflags = tc->flags;
        tc->flags &= ~TCF_IN_FOR_INIT;
        pn2 = AssignExpr(cx, ts, tc);
        tc->flags = oldflags | (tc->flags & TCF_FUN_FLAGS);

        if (!pn2)
            return NULL;
        MUST_MATCH_TOKEN(TOK_COLON, JSMSG_COLON_IN_COND);
        pn3 = AssignExpr(cx, ts, tc);
        if (!pn3)
            return NULL;
        pn->pn_pos.begin = pn1->pn_pos.begin;
        pn->pn_pos.end = pn3->pn_pos.end;
        pn->pn_kid1 = pn1;
        pn->pn_kid2 = pn2;
        pn->pn_kid3 = pn3;
    }
    return pn;
}

static JSParseNode *
OrExpr(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSParseNode *pn;

    pn = AndExpr(cx, ts, tc);
    while (pn && js_MatchToken(cx, ts, TOK_OR))
        pn = NewBinary(TOK_OR, JSOP_OR, pn, AndExpr(cx, ts, tc), tc);
    return pn;
}

static JSParseNode *
AndExpr(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSParseNode *pn;

    pn = BitOrExpr(cx, ts, tc);
    while (pn && js_MatchToken(cx, ts, TOK_AND))
        pn = NewBinary(TOK_AND, JSOP_AND, pn, BitOrExpr(cx, ts, tc), tc);
    return pn;
}

static JSParseNode *
BitOrExpr(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSParseNode *pn;

    pn = BitXorExpr(cx, ts, tc);
    while (pn && js_MatchToken(cx, ts, TOK_BITOR)) {
        pn = NewBinary(TOK_BITOR, JSOP_BITOR, pn, BitXorExpr(cx, ts, tc),
                       tc);
    }
    return pn;
}

static JSParseNode *
BitXorExpr(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSParseNode *pn;

    pn = BitAndExpr(cx, ts, tc);
    while (pn && js_MatchToken(cx, ts, TOK_BITXOR)) {
        pn = NewBinary(TOK_BITXOR, JSOP_BITXOR, pn, BitAndExpr(cx, ts, tc),
                       tc);
    }
    return pn;
}

static JSParseNode *
BitAndExpr(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSParseNode *pn;

    pn = EqExpr(cx, ts, tc);
    while (pn && js_MatchToken(cx, ts, TOK_BITAND))
        pn = NewBinary(TOK_BITAND, JSOP_BITAND, pn, EqExpr(cx, ts, tc), tc);
    return pn;
}

static JSParseNode *
EqExpr(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSParseNode *pn;
    JSOp op;

    pn = RelExpr(cx, ts, tc);
    while (pn && js_MatchToken(cx, ts, TOK_EQOP)) {
        op = CURRENT_TOKEN(ts).t_op;
        pn = NewBinary(TOK_EQOP, op, pn, RelExpr(cx, ts, tc), tc);
    }
    return pn;
}

static JSParseNode *
RelExpr(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSParseNode *pn;
    JSTokenType tt;
    JSOp op;
    uintN inForInitFlag = tc->flags & TCF_IN_FOR_INIT;

    /*
     * Uses of the in operator in ShiftExprs are always unambiguous,
     * so unset the flag that prohibits recognizing it.
     */
    tc->flags &= ~TCF_IN_FOR_INIT;

    pn = ShiftExpr(cx, ts, tc);
    while (pn &&
           (js_MatchToken(cx, ts, TOK_RELOP) ||
            /*
             * Recognize the 'in' token as an operator only if we're not
             * currently in the init expr of a for loop.
             */
            (inForInitFlag == 0 && js_MatchToken(cx, ts, TOK_IN)) ||
            js_MatchToken(cx, ts, TOK_INSTANCEOF))) {
        tt = CURRENT_TOKEN(ts).type;
        op = CURRENT_TOKEN(ts).t_op;
        pn = NewBinary(tt, op, pn, ShiftExpr(cx, ts, tc), tc);
    }
    /* Restore previous state of inForInit flag. */
    tc->flags |= inForInitFlag;

    return pn;
}

static JSParseNode *
ShiftExpr(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSParseNode *pn;
    JSOp op;

    pn = AddExpr(cx, ts, tc);
    while (pn && js_MatchToken(cx, ts, TOK_SHOP)) {
        op = CURRENT_TOKEN(ts).t_op;
        pn = NewBinary(TOK_SHOP, op, pn, AddExpr(cx, ts, tc), tc);
    }
    return pn;
}

static JSParseNode *
AddExpr(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSParseNode *pn;
    JSTokenType tt;
    JSOp op;

    pn = MulExpr(cx, ts, tc);
    while (pn &&
           (js_MatchToken(cx, ts, TOK_PLUS) ||
            js_MatchToken(cx, ts, TOK_MINUS))) {
        tt = CURRENT_TOKEN(ts).type;
        op = (tt == TOK_PLUS) ? JSOP_ADD : JSOP_SUB;
        pn = NewBinary(tt, op, pn, MulExpr(cx, ts, tc), tc);
    }
    return pn;
}

static JSParseNode *
MulExpr(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSParseNode *pn;
    JSTokenType tt;
    JSOp op;

    pn = UnaryExpr(cx, ts, tc);
    while (pn &&
           (js_MatchToken(cx, ts, TOK_STAR) ||
            js_MatchToken(cx, ts, TOK_DIVOP))) {
        tt = CURRENT_TOKEN(ts).type;
        op = CURRENT_TOKEN(ts).t_op;
        pn = NewBinary(tt, op, pn, UnaryExpr(cx, ts, tc), tc);
    }
    return pn;
}

static JSParseNode *
SetLvalKid(JSContext *cx, JSTokenStream *ts, JSParseNode *pn, JSParseNode *kid,
           const char *name)
{
    if (kid->pn_type != TOK_NAME &&
        kid->pn_type != TOK_DOT &&
        (kid->pn_type != TOK_LP ||
         (kid->pn_op != JSOP_CALL && kid->pn_op != JSOP_EVAL && kid->pn_op != JSOP_APPLY)) &&
#if JS_HAS_XML_SUPPORT
        (kid->pn_type != TOK_UNARYOP || kid->pn_op != JSOP_XMLNAME) &&
#endif
        kid->pn_type != TOK_LB) {
        js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                    JSMSG_BAD_OPERAND, name);
        return NULL;
    }
    pn->pn_kid = kid;
    return kid;
}

static const char incop_name_str[][10] = {"increment", "decrement"};

static JSBool
SetIncOpKid(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc,
            JSParseNode *pn, JSParseNode *kid,
            JSTokenType tt, JSBool preorder)
{
    JSOp op;

    kid = SetLvalKid(cx, ts, pn, kid, incop_name_str[tt == TOK_DEC]);
    if (!kid)
        return JS_FALSE;
    switch (kid->pn_type) {
      case TOK_NAME:
        op = (tt == TOK_INC)
             ? (preorder ? JSOP_INCNAME : JSOP_NAMEINC)
             : (preorder ? JSOP_DECNAME : JSOP_NAMEDEC);
        NoteLValue(cx, kid, tc);
        break;

      case TOK_DOT:
        op = (tt == TOK_INC)
             ? (preorder ? JSOP_INCPROP : JSOP_PROPINC)
             : (preorder ? JSOP_DECPROP : JSOP_PROPDEC);
        break;

      case TOK_LP:
        if (!MakeSetCall(cx, kid, tc, JSMSG_BAD_INCOP_OPERAND))
            return JS_FALSE;
        /* FALL THROUGH */
#if JS_HAS_XML_SUPPORT
      case TOK_UNARYOP:
        if (kid->pn_op == JSOP_XMLNAME)
            kid->pn_op = JSOP_SETXMLNAME;
        /* FALL THROUGH */
#endif
      case TOK_LB:
        op = (tt == TOK_INC)
             ? (preorder ? JSOP_INCELEM : JSOP_ELEMINC)
             : (preorder ? JSOP_DECELEM : JSOP_ELEMDEC);
        break;

      default:
        JS_ASSERT(0);
        op = JSOP_NOP;
    }
    pn->pn_op = op;
    return JS_TRUE;
}

static JSParseNode *
UnaryExpr(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSTokenType tt;
    JSParseNode *pn, *pn2;

    JS_CHECK_RECURSION(cx, return NULL);

    ts->flags |= TSF_OPERAND;
    tt = js_GetToken(cx, ts);
    ts->flags &= ~TSF_OPERAND;

    switch (tt) {
      case TOK_UNARYOP:
      case TOK_PLUS:
      case TOK_MINUS:
        pn = NewParseNode(PN_UNARY, tc);
        if (!pn)
            return NULL;
        pn->pn_type = TOK_UNARYOP;      /* PLUS and MINUS are binary */
        pn->pn_op = CURRENT_TOKEN(ts).t_op;
        pn2 = UnaryExpr(cx, ts, tc);
        if (!pn2)
            return NULL;
        pn->pn_pos.end = pn2->pn_pos.end;
        pn->pn_kid = pn2;
        break;

      case TOK_INC:
      case TOK_DEC:
        pn = NewParseNode(PN_UNARY, tc);
        if (!pn)
            return NULL;
        pn2 = MemberExpr(cx, ts, tc, JS_TRUE);
        if (!pn2)
            return NULL;
        if (!SetIncOpKid(cx, ts, tc, pn, pn2, tt, JS_TRUE))
            return NULL;
        pn->pn_pos.end = pn2->pn_pos.end;
        break;

      case TOK_DELETE:
        pn = NewParseNode(PN_UNARY, tc);
        if (!pn)
            return NULL;
        pn2 = UnaryExpr(cx, ts, tc);
        if (!pn2)
            return NULL;
        pn->pn_pos.end = pn2->pn_pos.end;

        /*
         * Under ECMA3, deleting any unary expression is valid -- it simply
         * returns true. Here we fold constants before checking for a call
         * expression, in order to rule out delete of a generator expression.
         */
        if (!js_FoldConstants(cx, pn2, tc))
            return NULL;
        switch (pn2->pn_type) {
          case TOK_LP:
            if (pn2->pn_op != JSOP_SETCALL &&
                !MakeSetCall(cx, pn2, tc, JSMSG_BAD_DELETE_OPERAND)) {
                return NULL;
            }
            break;
          case TOK_NAME:
            pn2->pn_op = JSOP_DELNAME;
            break;
          default:;
        }
        pn->pn_kid = pn2;
        break;

      case TOK_ERROR:
        return NULL;

      default:
        js_UngetToken(ts);
        pn = MemberExpr(cx, ts, tc, JS_TRUE);
        if (!pn)
            return NULL;

        /* Don't look across a newline boundary for a postfix incop. */
        if (ON_CURRENT_LINE(ts, pn->pn_pos)) {
            ts->flags |= TSF_OPERAND;
            tt = js_PeekTokenSameLine(cx, ts);
            ts->flags &= ~TSF_OPERAND;
            if (tt == TOK_INC || tt == TOK_DEC) {
                (void) js_GetToken(cx, ts);
                pn2 = NewParseNode(PN_UNARY, tc);
                if (!pn2)
                    return NULL;
                if (!SetIncOpKid(cx, ts, tc, pn2, pn, tt, JS_FALSE))
                    return NULL;
                pn2->pn_pos.begin = pn->pn_pos.begin;
                pn = pn2;
            }
        }
        break;
    }
    return pn;
}

#if JS_HAS_GENERATORS

/*
 * A dedicated helper for transplanting the comprehension expression E in
 *
 *   [E for (V in I)]   // array comprehension
 *   (E for (V in I))   // generator expression
 *
 * from its initial location in the AST, on the left of the 'for', to its final
 * position on the right. To avoid a separate pass we do this by adjusting the
 * blockids and name binding links that were established when E was parsed.
 *
 * A generator expression desugars like so:
 *
 *   (E for (V in I)) => (function () { for (var V in I) yield E; })()
 *
 * so the transplanter must adjust static level as well as blockid. E's source
 * coordinates in root->pn_pos are critical to deciding which binding links to
 * preserve and which to cut.
 *
 * NB: This is not a general tree transplanter -- it knows in particular that
 * the one or more bindings induced by V have not yet been created.
 */
class CompExprTransplanter {
    JSParseNode     *root;
    JSTreeContext   *tc;
    bool            genexp;
    uintN           adjust;
    uintN           funcLevel;

  public:
    CompExprTransplanter(JSParseNode *pn, JSTreeContext *tc, bool ge, uintN adj)
      : root(pn), tc(tc), genexp(ge), adjust(adj), funcLevel(0)
    {
    }

    bool transplant(JSParseNode *pn);
};

/*
 * Any definitions nested within the comprehension expression of a generator
 * expression must move "down" one static level, which of course increases the
 * upvar-frame-skip count.
 */
static bool
BumpStaticLevel(JSParseNode *pn, JSTreeContext *tc)
{
    if (pn->pn_cookie != FREE_UPVAR_COOKIE) {
        uintN level = UPVAR_FRAME_SKIP(pn->pn_cookie) + 1;

        JS_ASSERT(level >= tc->staticLevel);
        if (level >= FREE_STATIC_LEVEL) {
            JS_ReportErrorNumber(tc->compiler->context, js_GetErrorMessage, NULL,
                                 JSMSG_TOO_DEEP, js_function_str);
            return false;
        }

        pn->pn_cookie = MAKE_UPVAR_COOKIE(level, UPVAR_FRAME_SLOT(pn->pn_cookie));
    }
    return true;
}

static void
AdjustBlockId(JSParseNode *pn, uintN adjust, JSTreeContext *tc)
{
    JS_ASSERT(pn->pn_arity == PN_LIST || pn->pn_arity == PN_FUNC || pn->pn_arity == PN_NAME);
    pn->pn_blockid += adjust;
    if (pn->pn_blockid >= tc->blockidGen)
        tc->blockidGen = pn->pn_blockid + 1;
}

bool
CompExprTransplanter::transplant(JSParseNode *pn)
{
    if (!pn)
        return true;

    switch (pn->pn_arity) {
      case PN_LIST:
        for (JSParseNode *pn2 = pn->pn_head; pn2; pn2 = pn2->pn_next)
            transplant(pn2);
        if (pn->pn_pos >= root->pn_pos)
            AdjustBlockId(pn, adjust, tc);
        break;

      case PN_TERNARY:
        transplant(pn->pn_kid1);
        transplant(pn->pn_kid2);
        transplant(pn->pn_kid3);
        break;

      case PN_BINARY:
        transplant(pn->pn_left);

        /* Binary TOK_COLON nodes can have left == right. See bug 492714. */
        if (pn->pn_right != pn->pn_left)
            transplant(pn->pn_right);
        break;

      case PN_UNARY:
        transplant(pn->pn_kid);
        break;

      case PN_FUNC:
      {
        /*
         * Only the first level of transplant recursion through functions needs
         * to reparent the funbox, since all descendant functions are correctly
         * linked under the top-most funbox. But every visit to this case needs
         * to update funbox->level.
         *
         * Recall that funbox->level is the static level of the code containing
         * the definition or expression of the function and not the static level
         * of the function's body.
         */
        JSFunctionBox *funbox = pn->pn_funbox;

        funbox->level = tc->staticLevel + funcLevel;
        if (++funcLevel == 1 && genexp) {
            JSFunctionBox *parent = tc->funbox;

            JSFunctionBox **funboxp = &tc->parent->functionList;
            while (*funboxp != funbox)
                funboxp = &(*funboxp)->siblings;
            *funboxp = funbox->siblings;

            funbox->parent = parent;
            funbox->siblings = parent->kids;
            parent->kids = funbox;
            funbox->level = tc->staticLevel;
        }
        /* FALL THROUGH */
      }

      case PN_NAME:
        transplant(pn->maybeExpr());
        if (pn->pn_arity == PN_FUNC)
            --funcLevel;

        if (pn->pn_defn) {
            if (genexp && !BumpStaticLevel(pn, tc))
                return false;
        } else if (pn->pn_used) {
            JS_ASSERT(pn->pn_op != JSOP_NOP);
            JS_ASSERT(pn->pn_cookie == FREE_UPVAR_COOKIE);

            JSDefinition *dn = pn->pn_lexdef;
            JS_ASSERT(dn->pn_defn);

            /*
             * Adjust the definition's block id only if it is a placeholder not
             * to the left of the root node, and if pn is the last use visited
             * in the comprehension expression (to avoid adjusting the blockid
             * multiple times).
             *
             * Non-placeholder definitions within the comprehension expression
             * will be visited further below.
             */
            if (dn->isPlaceholder() && dn->pn_pos >= root->pn_pos && dn->dn_uses == pn) {
                if (genexp && !BumpStaticLevel(dn, tc))
                    return false;
                AdjustBlockId(dn, adjust, tc);
            }

            JSAtom *atom = pn->pn_atom;
#ifdef DEBUG
            JSStmtInfo *stmt = js_LexicalLookup(tc, atom, NULL);
            JS_ASSERT(!stmt || stmt != tc->topStmt);
#endif
            if (genexp && PN_OP(dn) != JSOP_CALLEE) {
                JS_ASSERT(!tc->decls.lookup(atom));

                if (dn->pn_pos < root->pn_pos || dn->isPlaceholder()) {
                    JSAtomListElement *ale = tc->lexdeps.add(tc->compiler, dn->pn_atom);
                    if (!ale)
                        return false;

                    if (dn->pn_pos >= root->pn_pos) {
                        tc->parent->lexdeps.remove(tc->compiler, atom);
                    } else {
                        JSDefinition *dn2 = (JSDefinition *)
                            NewNameNode(tc->compiler->context, dn->pn_atom, tc);
                        if (!dn2)
                            return false;

                        dn2->pn_type = dn->pn_type;
                        dn2->pn_pos = root->pn_pos;
                        dn2->pn_defn = true;
                        dn2->pn_dflags |= PND_PLACEHOLDER;

                        JSParseNode **pnup = &dn->dn_uses;
                        JSParseNode *pnu;
                        while ((pnu = *pnup) != NULL && pnu->pn_pos >= root->pn_pos) {
                            pnu->pn_lexdef = dn2;
                            dn2->pn_dflags |= pnu->pn_dflags & PND_USE2DEF_FLAGS;
                            pnup = &pnu->pn_link;
                        }
                        dn2->dn_uses = dn->dn_uses;
                        dn->dn_uses = *pnup;
                        *pnup = NULL;

                        dn = dn2;
                    }

                    ALE_SET_DEFN(ale, dn);
                }
            }
        }

        if (pn->pn_pos >= root->pn_pos)
            AdjustBlockId(pn, adjust, tc);
        break;

      case PN_NAMESET:
        transplant(pn->pn_tree);
        break;
    }
    return true;
}

/*
 * Starting from a |for| keyword after the first array initialiser element or
 * an expression in an open parenthesis, parse the tail of the comprehension
 * or generator expression signified by this |for| keyword in context.
 *
 * Return null on failure, else return the top-most parse node for the array
 * comprehension or generator expression, with a unary node as the body of the
 * (possibly nested) for-loop, initialized by |type, op, kid|.
 */
static JSParseNode *
ComprehensionTail(JSParseNode *kid, uintN blockid, JSTreeContext *tc,
                  JSTokenType type = TOK_SEMI, JSOp op = JSOP_NOP)
{
    JSContext *cx = tc->compiler->context;
    JSTokenStream *ts = TS(tc->compiler);

    uintN adjust;
    JSParseNode *pn, *pn2, *pn3, **pnp;
    JSStmtInfo stmtInfo;
    BindData data;
    JSTokenType tt;
    JSAtom *atom;

    JS_ASSERT(CURRENT_TOKEN(ts).type == TOK_FOR);

    if (type == TOK_SEMI) {
        /*
         * Generator expression desugars to an immediately applied lambda that
         * yields the next value from a for-in loop (possibly nested, and with
         * optional if guard). Make pn be the TOK_LC body node.
         */
        pn = PushLexicalScope(cx, ts, tc, &stmtInfo);
        if (!pn)
            return NULL;
        adjust = pn->pn_blockid - blockid;
    } else {
        JS_ASSERT(type == TOK_ARRAYPUSH);

        /*
         * Make a parse-node and literal object representing the block scope of
         * this array comprehension. Our caller in PrimaryExpr, the TOK_LB case
         * aka the array initialiser case, has passed the blockid to claim for
         * the comprehension's block scope. We allocate that id or one above it
         * here, by calling js_PushLexicalScope.
         *
         * In the case of a comprehension expression that has nested blocks
         * (e.g., let expressions), we will allocate a higher blockid but then
         * slide all blocks "to the right" to make room for the comprehension's
         * block scope.
         */
        adjust = tc->blockid();
        pn = PushLexicalScope(cx, ts, tc, &stmtInfo);
        if (!pn)
            return NULL;

        JS_ASSERT(blockid <= pn->pn_blockid);
        JS_ASSERT(blockid < tc->blockidGen);
        JS_ASSERT(tc->bodyid < blockid);
        pn->pn_blockid = stmtInfo.blockid = blockid;
        JS_ASSERT(adjust < blockid);
        adjust = blockid - adjust;
    }

    pnp = &pn->pn_expr;

    CompExprTransplanter transplanter(kid, tc, type == TOK_SEMI, adjust);
    transplanter.transplant(kid);

    data.pn = NULL;
    data.op = JSOP_NOP;
    data.binder = BindLet;
    data.let.overflow = JSMSG_ARRAY_INIT_TOO_BIG;

    do {
        /*
         * FOR node is binary, left is loop control and right is body.  Use
         * index to count each block-local let-variable on the left-hand side
         * of the IN.
         */
        pn2 = NewParseNode(PN_BINARY, tc);
        if (!pn2)
            return NULL;

        pn2->pn_op = JSOP_ITER;
        pn2->pn_iflags = JSITER_ENUMERATE;
        if (js_MatchToken(cx, ts, TOK_NAME)) {
            if (CURRENT_TOKEN(ts).t_atom == cx->runtime->atomState.eachAtom)
                pn2->pn_iflags |= JSITER_FOREACH;
            else
                js_UngetToken(ts);
        }
        MUST_MATCH_TOKEN(TOK_LP, JSMSG_PAREN_AFTER_FOR);

        atom = NULL;
        tt = js_GetToken(cx, ts);
        switch (tt) {
#if JS_HAS_DESTRUCTURING
          case TOK_LB:
          case TOK_LC:
            tc->flags |= TCF_DECL_DESTRUCTURING;
            pn3 = PrimaryExpr(cx, ts, tc, tt, JS_FALSE);
            tc->flags &= ~TCF_DECL_DESTRUCTURING;
            if (!pn3)
                return NULL;
            break;
#endif

          case TOK_NAME:
            atom = CURRENT_TOKEN(ts).t_atom;

            /*
             * Create a name node with pn_op JSOP_NAME.  We can't set pn_op to
             * JSOP_GETLOCAL here, because we don't yet know the block's depth
             * in the operand stack frame.  The code generator computes that,
             * and it tries to bind all names to slots, so we must let it do
             * the deed.
             */
            pn3 = NewBindingNode(atom, tc, true);
            if (!pn3)
                return NULL;
            break;

          default:
            js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                        JSMSG_NO_VARIABLE_NAME);

          case TOK_ERROR:
            return NULL;
        }

        MUST_MATCH_TOKEN(TOK_IN, JSMSG_IN_AFTER_FOR_NAME);
        JSParseNode *pn4 = Expr(cx, ts, tc);
        if (!pn4)
            return NULL;
        MUST_MATCH_TOKEN(TOK_RP, JSMSG_PAREN_AFTER_FOR_CTRL);

        switch (tt) {
#if JS_HAS_DESTRUCTURING
          case TOK_LB:
          case TOK_LC:
            if (!CheckDestructuring(cx, &data, pn3, NULL, tc))
                return NULL;

            if (JSVERSION_NUMBER(cx) == JSVERSION_1_7) {
                /* Destructuring requires [key, value] enumeration in JS1.7. */
                if (pn3->pn_type != TOK_RB || pn3->pn_count != 2) {
                    js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                                JSMSG_BAD_FOR_LEFTSIDE);
                    return NULL;
                }

                JS_ASSERT(pn2->pn_op == JSOP_ITER);
                JS_ASSERT(pn2->pn_iflags & JSITER_ENUMERATE);
                if (!(pn2->pn_iflags & JSITER_FOREACH))
                    pn2->pn_iflags |= JSITER_FOREACH | JSITER_KEYVALUE;
            }
            break;
#endif

          case TOK_NAME:
            data.pn = pn3;
            if (!data.binder(cx, &data, atom, tc))
                return NULL;
            break;

          default:;
        }

        pn2->pn_left = NewBinary(TOK_IN, JSOP_NOP, pn3, pn4, tc);
        if (!pn2->pn_left)
            return NULL;
        *pnp = pn2;
        pnp = &pn2->pn_right;
    } while (js_MatchToken(cx, ts, TOK_FOR));

    if (js_MatchToken(cx, ts, TOK_IF)) {
        pn2 = NewParseNode(PN_TERNARY, tc);
        if (!pn2)
            return NULL;
        pn2->pn_kid1 = Condition(cx, ts, tc);
        if (!pn2->pn_kid1)
            return NULL;
        *pnp = pn2;
        pnp = &pn2->pn_kid2;
    }

    pn2 = NewParseNode(PN_UNARY, tc);
    if (!pn2)
        return NULL;
    pn2->pn_type = type;
    pn2->pn_op = op;
    pn2->pn_kid = kid;
    *pnp = pn2;

    if (type == TOK_ARRAYPUSH)
        PopStatement(tc);
    return pn;
}

#if JS_HAS_GENERATOR_EXPRS

/*
 * Starting from a |for| keyword after an expression, parse the comprehension
 * tail completing this generator expression. Wrap the expression at kid in a
 * generator function that is immediately called to evaluate to the generator
 * iterator that is the value of this generator expression.
 *
 * Callers pass a blank unary node via pn, which GeneratorExpr fills in as the
 * yield expression, which ComprehensionTail in turn wraps in a TOK_SEMI-type
 * expression-statement node that constitutes the body of the |for| loop(s) in
 * the generator function.
 *
 * Note how unlike Python, we do not evaluate the expression to the right of
 * the first |in| in the chain of |for| heads. Instead, a generator expression
 * is merely sugar for a generator function expression and its application.
 */
static JSParseNode *
GeneratorExpr(JSParseNode *pn, JSParseNode *kid, JSTreeContext *tc)
{
    /* Initialize pn, connecting it to kid. */
    JS_ASSERT(pn->pn_arity == PN_UNARY);
    pn->pn_type = TOK_YIELD;
    pn->pn_op = JSOP_YIELD;
    pn->pn_parens = true;
    pn->pn_pos = kid->pn_pos;
    pn->pn_kid = kid;
    pn->pn_hidden = true;

    /* Make a new node for the desugared generator function. */
    JSParseNode *genfn = NewParseNode(PN_FUNC, tc);
    if (!genfn)
        return NULL;
    genfn->pn_type = TOK_FUNCTION;
    genfn->pn_op = JSOP_LAMBDA;
    JS_ASSERT(!genfn->pn_body);
    genfn->pn_dflags = PND_FUNARG;

    {
        JSTreeContext gentc(tc->compiler);

        JSFunctionBox *funbox = EnterFunction(genfn, tc, &gentc);
        if (!funbox)
            return NULL;

        /*
         * We have to dance around a bit to propagate sharp variables from tc
         * to gentc before setting TCF_HAS_SHARPS implicitly by propagating all
         * of tc's TCF_FUN_FLAGS flags. As below, we have to be conservative by
         * leaving TCF_HAS_SHARPS set in tc if we do propagate to gentc.
         */
        if (tc->flags & TCF_HAS_SHARPS) {
            gentc.flags |= TCF_IN_FUNCTION;
            if (!gentc.ensureSharpSlots())
                return NULL;
        }

        /*
         * We assume conservatively that any deoptimization flag in tc->flags
         * besides TCF_FUN_PARAM_ARGUMENTS can come from the kid. So we
         * propagate these flags into genfn. For code simplicity we also do
         * not detect if the flags were only set in the kid and could be
         * removed from tc->flags.
         */
        gentc.flags |= TCF_FUN_IS_GENERATOR | TCF_GENEXP_LAMBDA |
                       (tc->flags & (TCF_FUN_FLAGS & ~TCF_FUN_PARAM_ARGUMENTS));
        funbox->tcflags |= gentc.flags;
        genfn->pn_funbox = funbox;
        genfn->pn_blockid = gentc.bodyid;

        JSParseNode *body = ComprehensionTail(pn, tc->blockid(), &gentc);
        if (!body)
            return NULL;
        JS_ASSERT(!genfn->pn_body);
        genfn->pn_body = body;
        genfn->pn_pos.begin = body->pn_pos.begin = kid->pn_pos.begin;
        genfn->pn_pos.end = body->pn_pos.end = CURRENT_TOKEN(TS(tc->compiler)).pos.end;

        if (!LeaveFunction(genfn, &gentc, tc))
            return NULL;
    }

    /*
     * Our result is a call expression that invokes the anonymous generator
     * function object.
     */
    JSParseNode *result = NewParseNode(PN_LIST, tc);
    if (!result)
        return NULL;
    result->pn_type = TOK_LP;
    result->pn_op = JSOP_CALL;
    result->pn_pos.begin = genfn->pn_pos.begin;
    result->initList(genfn);
    return result;
}

static const char js_generator_str[] = "generator";

#endif /* JS_HAS_GENERATOR_EXPRS */
#endif /* JS_HAS_GENERATORS */

static JSBool
ArgumentList(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc,
             JSParseNode *listNode)
{
    JSBool matched;

    ts->flags |= TSF_OPERAND;
    matched = js_MatchToken(cx, ts, TOK_RP);
    ts->flags &= ~TSF_OPERAND;
    if (!matched) {
        do {
            JSParseNode *argNode = AssignExpr(cx, ts, tc);
            if (!argNode)
                return JS_FALSE;
#if JS_HAS_GENERATORS
            if (argNode->pn_type == TOK_YIELD &&
                !argNode->pn_parens &&
                js_PeekToken(cx, ts) == TOK_COMMA) {
                js_ReportCompileErrorNumber(cx, ts, argNode, JSREPORT_ERROR,
                                            JSMSG_BAD_GENERATOR_SYNTAX,
                                            js_yield_str);
                return JS_FALSE;
            }
#endif
#if JS_HAS_GENERATOR_EXPRS
            if (js_MatchToken(cx, ts, TOK_FOR)) {
                JSParseNode *pn = NewParseNode(PN_UNARY, tc);
                if (!pn)
                    return JS_FALSE;
                argNode = GeneratorExpr(pn, argNode, tc);
                if (!argNode)
                    return JS_FALSE;
                if (listNode->pn_count > 1 ||
                    js_PeekToken(cx, ts) == TOK_COMMA) {
                    js_ReportCompileErrorNumber(cx, ts, argNode, JSREPORT_ERROR,
                                                JSMSG_BAD_GENERATOR_SYNTAX,
                                                js_generator_str);
                    return JS_FALSE;
                }
            }
#endif
            listNode->append(argNode);
        } while (js_MatchToken(cx, ts, TOK_COMMA));

        if (js_GetToken(cx, ts) != TOK_RP) {
            js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                        JSMSG_PAREN_AFTER_ARGS);
            return JS_FALSE;
        }
    }
    return JS_TRUE;
}

/* Check for an immediately-applied (new'ed) lambda and clear PND_FUNARG. */
static JSParseNode *
CheckForImmediatelyAppliedLambda(JSParseNode *pn)
{
    while (pn->pn_type == TOK_RP)
        pn = pn->pn_kid;
    if (pn->pn_type == TOK_FUNCTION) {
        JS_ASSERT(pn->pn_arity == PN_FUNC);

        JSFunctionBox *funbox = pn->pn_funbox;
        JS_ASSERT(((JSFunction *) funbox->object)->flags & JSFUN_LAMBDA);
        if (!(funbox->tcflags & (TCF_FUN_USES_ARGUMENTS | TCF_FUN_USES_OWN_NAME)))
            pn->pn_dflags &= ~PND_FUNARG;
    }
    return pn;
}

static JSParseNode *
MemberExpr(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc,
           JSBool allowCallSyntax)
{
    JSParseNode *pn, *pn2, *pn3;
    JSTokenType tt;

    JS_CHECK_RECURSION(cx, return NULL);

    /* Check for new expression first. */
    ts->flags |= TSF_OPERAND;
    tt = js_GetToken(cx, ts);
    ts->flags &= ~TSF_OPERAND;
    if (tt == TOK_NEW) {
        pn = NewParseNode(PN_LIST, tc);
        if (!pn)
            return NULL;
        pn2 = MemberExpr(cx, ts, tc, JS_FALSE);
        if (!pn2)
            return NULL;
        pn2 = CheckForImmediatelyAppliedLambda(pn2);
        pn->pn_op = JSOP_NEW;
        pn->initList(pn2);
        pn->pn_pos.begin = pn2->pn_pos.begin;

        if (js_MatchToken(cx, ts, TOK_LP) && !ArgumentList(cx, ts, tc, pn))
            return NULL;
        if (pn->pn_count > ARGC_LIMIT) {
            JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                 JSMSG_TOO_MANY_CON_ARGS);
            return NULL;
        }
        pn->pn_pos.end = pn->last()->pn_pos.end;
    } else {
        pn = PrimaryExpr(cx, ts, tc, tt, JS_FALSE);
        if (!pn)
            return NULL;

        if (pn->pn_type == TOK_ANYNAME ||
            pn->pn_type == TOK_AT ||
            pn->pn_type == TOK_DBLCOLON) {
            pn2 = NewOrRecycledNode(tc);
            if (!pn2)
                return NULL;
            pn2->pn_type = TOK_UNARYOP;
            pn2->pn_pos = pn->pn_pos;
            pn2->pn_op = JSOP_XMLNAME;
            pn2->pn_arity = PN_UNARY;
            pn2->pn_parens = false;
            pn2->pn_kid = pn;
            pn = pn2;
        }
    }

    while ((tt = js_GetToken(cx, ts)) > TOK_EOF) {
        if (tt == TOK_DOT) {
            pn2 = NewNameNode(cx, NULL, tc);
            if (!pn2)
                return NULL;
#if JS_HAS_XML_SUPPORT
            ts->flags |= TSF_OPERAND | TSF_KEYWORD_IS_NAME;
            tt = js_GetToken(cx, ts);
            ts->flags &= ~(TSF_OPERAND | TSF_KEYWORD_IS_NAME);
            pn3 = PrimaryExpr(cx, ts, tc, tt, JS_TRUE);
            if (!pn3)
                return NULL;

            /* Check both tt and pn_type, to distinguish |x.(y)| and |x.y::z| from |x.y|. */
            if (tt == TOK_NAME && pn3->pn_type == TOK_NAME) {
                pn2->pn_op = JSOP_GETPROP;
                pn2->pn_expr = pn;
                pn2->pn_atom = pn3->pn_atom;
                RecycleTree(pn3, tc);
            } else {
                if (tt == TOK_LP) {
                    pn2->pn_type = TOK_FILTER;
                    pn2->pn_op = JSOP_FILTER;

                    /* A filtering predicate is like a with statement. */
                    tc->flags |= TCF_FUN_HEAVYWEIGHT;
                } else if (TOKEN_TYPE_IS_XML(PN_TYPE(pn3))) {
                    pn2->pn_type = TOK_LB;
                    pn2->pn_op = JSOP_GETELEM;
                } else {
                    js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                                JSMSG_NAME_AFTER_DOT);
                    return NULL;
                }
                pn2->pn_arity = PN_BINARY;
                pn2->pn_left = pn;
                pn2->pn_right = pn3;
            }
#else
            ts->flags |= TSF_KEYWORD_IS_NAME;
            MUST_MATCH_TOKEN(TOK_NAME, JSMSG_NAME_AFTER_DOT);
            ts->flags &= ~TSF_KEYWORD_IS_NAME;
            pn2->pn_op = JSOP_GETPROP;
            pn2->pn_expr = pn;
            pn2->pn_atom = CURRENT_TOKEN(ts).t_atom;
#endif
            pn2->pn_pos.begin = pn->pn_pos.begin;
            pn2->pn_pos.end = CURRENT_TOKEN(ts).pos.end;
#if JS_HAS_XML_SUPPORT
        } else if (tt == TOK_DBLDOT) {
            pn2 = NewParseNode(PN_BINARY, tc);
            if (!pn2)
                return NULL;
            ts->flags |= TSF_OPERAND | TSF_KEYWORD_IS_NAME;
            tt = js_GetToken(cx, ts);
            ts->flags &= ~(TSF_OPERAND | TSF_KEYWORD_IS_NAME);
            pn3 = PrimaryExpr(cx, ts, tc, tt, JS_TRUE);
            if (!pn3)
                return NULL;
            tt = PN_TYPE(pn3);
            if (tt == TOK_NAME && !pn3->pn_parens) {
                pn3->pn_type = TOK_STRING;
                pn3->pn_arity = PN_NULLARY;
                pn3->pn_op = JSOP_QNAMEPART;
            } else if (!TOKEN_TYPE_IS_XML(tt)) {
                js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                            JSMSG_NAME_AFTER_DOT);
                return NULL;
            }
            pn2->pn_op = JSOP_DESCENDANTS;
            pn2->pn_left = pn;
            pn2->pn_right = pn3;
            pn2->pn_pos.begin = pn->pn_pos.begin;
            pn2->pn_pos.end = CURRENT_TOKEN(ts).pos.end;
#endif
        } else if (tt == TOK_LB) {
            pn2 = NewParseNode(PN_BINARY, tc);
            if (!pn2)
                return NULL;
            pn3 = Expr(cx, ts, tc);
            if (!pn3)
                return NULL;

            MUST_MATCH_TOKEN(TOK_RB, JSMSG_BRACKET_IN_INDEX);
            pn2->pn_pos.begin = pn->pn_pos.begin;
            pn2->pn_pos.end = CURRENT_TOKEN(ts).pos.end;

            /*
             * Optimize o['p'] to o.p by rewriting pn2, but avoid rewriting
             * o['0'] to use JSOP_GETPROP, to keep fast indexing disjoint in
             * the interpreter from fast property access. However, if the
             * bracketed string is a uint32, we rewrite pn3 to be a number
             * instead of a string.
             */
            do {
                if (pn3->pn_type == TOK_STRING) {
                    jsuint index;

                    if (!js_IdIsIndex(ATOM_TO_JSID(pn3->pn_atom), &index)) {
                        pn2->pn_type = TOK_DOT;
                        pn2->pn_op = JSOP_GETPROP;
                        pn2->pn_arity = PN_NAME;
                        pn2->pn_expr = pn;
                        pn2->pn_atom = pn3->pn_atom;
                        break;
                    }
                    pn3->pn_type = TOK_NUMBER;
                    pn3->pn_op = JSOP_DOUBLE;
                    pn3->pn_dval = index;
                }
                pn2->pn_op = JSOP_GETELEM;
                pn2->pn_left = pn;
                pn2->pn_right = pn3;
            } while (0);
        } else if (allowCallSyntax && tt == TOK_LP) {
            pn2 = NewParseNode(PN_LIST, tc);
            if (!pn2)
                return NULL;
            pn2->pn_op = JSOP_CALL;

            /* CheckForImmediatelyAppliedLambda skips useless TOK_RP nodes. */
            pn = CheckForImmediatelyAppliedLambda(pn);
            if (pn->pn_op == JSOP_NAME) {
                if (pn->pn_atom == cx->runtime->atomState.evalAtom) {
                    /* Select JSOP_EVAL and flag tc as heavyweight. */
                    pn2->pn_op = JSOP_EVAL;
                    tc->flags |= TCF_FUN_HEAVYWEIGHT;
                }
            } else if (pn->pn_op == JSOP_GETPROP) {
                if (pn->pn_atom == cx->runtime->atomState.applyAtom ||
                    pn->pn_atom == cx->runtime->atomState.callAtom) {
                    /* Select JSOP_APPLY given foo.apply(...). */
                    pn2->pn_op = JSOP_APPLY;
                }
            }

            pn2->initList(pn);
            pn2->pn_pos.begin = pn->pn_pos.begin;

            if (!ArgumentList(cx, ts, tc, pn2))
                return NULL;
            if (pn2->pn_count > ARGC_LIMIT) {
                JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL,
                                     JSMSG_TOO_MANY_FUN_ARGS);
                return NULL;
            }
            pn2->pn_pos.end = CURRENT_TOKEN(ts).pos.end;
        } else {
            js_UngetToken(ts);
            return pn;
        }

        pn = pn2;
    }
    if (tt == TOK_ERROR)
        return NULL;
    return pn;
}

static JSParseNode *
BracketedExpr(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    uintN oldflags;
    JSParseNode *pn;

    /*
     * Always accept the 'in' operator in a parenthesized expression,
     * where it's unambiguous, even if we might be parsing the init of a
     * for statement.
     */
    oldflags = tc->flags;
    tc->flags &= ~TCF_IN_FOR_INIT;
    pn = Expr(cx, ts, tc);
    tc->flags = oldflags | (tc->flags & TCF_FUN_FLAGS);
    return pn;
}

#if JS_HAS_XML_SUPPORT

static JSParseNode *
EndBracketedExpr(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSParseNode *pn;

    pn = BracketedExpr(cx, ts, tc);
    if (!pn)
        return NULL;

    MUST_MATCH_TOKEN(TOK_RB, JSMSG_BRACKET_AFTER_ATTR_EXPR);
    return pn;
}

/*
 * From the ECMA-357 grammar in 11.1.1 and 11.1.2:
 *
 *      AttributeIdentifier:
 *              @ PropertySelector
 *              @ QualifiedIdentifier
 *              @ [ Expression ]
 *
 *      PropertySelector:
 *              Identifier
 *              *
 *
 *      QualifiedIdentifier:
 *              PropertySelector :: PropertySelector
 *              PropertySelector :: [ Expression ]
 *
 * We adapt AttributeIdentifier and QualifiedIdentier to be LL(1), like so:
 *
 *      AttributeIdentifier:
 *              @ QualifiedIdentifier
 *              @ [ Expression ]
 *
 *      PropertySelector:
 *              Identifier
 *              *
 *
 *      QualifiedIdentifier:
 *              PropertySelector :: PropertySelector
 *              PropertySelector :: [ Expression ]
 *              PropertySelector
 *
 * As PrimaryExpression: Identifier is in ECMA-262 and we want the semantics
 * for that rule to result in a name node, but ECMA-357 extends the grammar
 * to include PrimaryExpression: QualifiedIdentifier, we must factor further:
 *
 *      QualifiedIdentifier:
 *              PropertySelector QualifiedSuffix
 *
 *      QualifiedSuffix:
 *              :: PropertySelector
 *              :: [ Expression ]
 *              /nothing/
 *
 * And use this production instead of PrimaryExpression: QualifiedIdentifier:
 *
 *      PrimaryExpression:
 *              Identifier QualifiedSuffix
 *
 * We hoist the :: match into callers of QualifiedSuffix, in order to tweak
 * PropertySelector vs. Identifier pn_arity, pn_op, and other members.
 */
static JSParseNode *
PropertySelector(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSParseNode *pn;

    pn = NewParseNode(PN_NULLARY, tc);
    if (!pn)
        return NULL;
    if (pn->pn_type == TOK_STAR) {
        pn->pn_type = TOK_ANYNAME;
        pn->pn_op = JSOP_ANYNAME;
        pn->pn_atom = cx->runtime->atomState.starAtom;
    } else {
        JS_ASSERT(pn->pn_type == TOK_NAME);
        pn->pn_op = JSOP_QNAMEPART;
        pn->pn_arity = PN_NAME;
        pn->pn_atom = CURRENT_TOKEN(ts).t_atom;
        pn->pn_cookie = FREE_UPVAR_COOKIE;
    }
    return pn;
}

static JSParseNode *
QualifiedSuffix(JSContext *cx, JSTokenStream *ts, JSParseNode *pn,
                JSTreeContext *tc)
{
    JSParseNode *pn2, *pn3;
    JSTokenType tt;

    JS_ASSERT(CURRENT_TOKEN(ts).type == TOK_DBLCOLON);
    pn2 = NewNameNode(cx, NULL, tc);
    if (!pn2)
        return NULL;

    /* Left operand of :: must be evaluated if it is an identifier. */
    if (pn->pn_op == JSOP_QNAMEPART)
        pn->pn_op = JSOP_NAME;

    ts->flags |= TSF_KEYWORD_IS_NAME;
    tt = js_GetToken(cx, ts);
    ts->flags &= ~TSF_KEYWORD_IS_NAME;
    if (tt == TOK_STAR || tt == TOK_NAME) {
        /* Inline and specialize PropertySelector for JSOP_QNAMECONST. */
        pn2->pn_op = JSOP_QNAMECONST;
        pn2->pn_pos.begin = pn->pn_pos.begin;
        pn2->pn_atom = (tt == TOK_STAR)
                       ? cx->runtime->atomState.starAtom
                       : CURRENT_TOKEN(ts).t_atom;
        pn2->pn_expr = pn;
        pn2->pn_cookie = FREE_UPVAR_COOKIE;
        return pn2;
    }

    if (tt != TOK_LB) {
        js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                    JSMSG_SYNTAX_ERROR);
        return NULL;
    }
    pn3 = EndBracketedExpr(cx, ts, tc);
    if (!pn3)
        return NULL;

    pn2->pn_op = JSOP_QNAME;
    pn2->pn_arity = PN_BINARY;
    pn2->pn_pos.begin = pn->pn_pos.begin;
    pn2->pn_pos.end = pn3->pn_pos.end;
    pn2->pn_left = pn;
    pn2->pn_right = pn3;
    return pn2;
}

static JSParseNode *
QualifiedIdentifier(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSParseNode *pn;

    pn = PropertySelector(cx, ts, tc);
    if (!pn)
        return NULL;
    if (js_MatchToken(cx, ts, TOK_DBLCOLON)) {
        /* Hack for bug 496316. Slowing down E4X won't make it go away, alas. */
        tc->flags |= TCF_FUN_HEAVYWEIGHT;
        pn = QualifiedSuffix(cx, ts, pn, tc);
    }
    return pn;
}

static JSParseNode *
AttributeIdentifier(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSParseNode *pn, *pn2;
    JSTokenType tt;

    JS_ASSERT(CURRENT_TOKEN(ts).type == TOK_AT);
    pn = NewParseNode(PN_UNARY, tc);
    if (!pn)
        return NULL;
    pn->pn_op = JSOP_TOATTRNAME;
    ts->flags |= TSF_KEYWORD_IS_NAME;
    tt = js_GetToken(cx, ts);
    ts->flags &= ~TSF_KEYWORD_IS_NAME;
    if (tt == TOK_STAR || tt == TOK_NAME) {
        pn2 = QualifiedIdentifier(cx, ts, tc);
    } else if (tt == TOK_LB) {
        pn2 = EndBracketedExpr(cx, ts, tc);
    } else {
        js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                    JSMSG_SYNTAX_ERROR);
        return NULL;
    }
    if (!pn2)
        return NULL;
    pn->pn_kid = pn2;
    return pn;
}

/*
 * Make a TOK_LC unary node whose pn_kid is an expression.
 */
static JSParseNode *
XMLExpr(JSContext *cx, JSTokenStream *ts, JSBool inTag, JSTreeContext *tc)
{
    JSParseNode *pn, *pn2;
    uintN oldflags;

    JS_ASSERT(CURRENT_TOKEN(ts).type == TOK_LC);
    pn = NewParseNode(PN_UNARY, tc);
    if (!pn)
        return NULL;

    /*
     * Turn off XML tag mode, but don't restore it after parsing this braced
     * expression.  Instead, simply restore ts's old flags.  This is required
     * because XMLExpr is called both from within a tag, and from within text
     * contained in an element, but outside of any start, end, or point tag.
     */
    oldflags = ts->flags;
    ts->flags = oldflags & ~TSF_XMLTAGMODE;
    pn2 = Expr(cx, ts, tc);
    if (!pn2)
        return NULL;

    MUST_MATCH_TOKEN(TOK_RC, JSMSG_CURLY_IN_XML_EXPR);
    ts->flags = oldflags;
    pn->pn_kid = pn2;
    pn->pn_op = inTag ? JSOP_XMLTAGEXPR : JSOP_XMLELTEXPR;
    return pn;
}

/*
 * Make a terminal node for one of TOK_XMLNAME, TOK_XMLATTR, TOK_XMLSPACE,
 * TOK_XMLTEXT, TOK_XMLCDATA, TOK_XMLCOMMENT, or TOK_XMLPI.  When converting
 * parse tree to XML, we preserve a TOK_XMLSPACE node only if it's the sole
 * child of a container tag.
 */
static JSParseNode *
XMLAtomNode(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSParseNode *pn;
    JSToken *tp;

    pn = NewParseNode(PN_NULLARY, tc);
    if (!pn)
        return NULL;
    tp = &CURRENT_TOKEN(ts);
    pn->pn_op = tp->t_op;
    pn->pn_atom = tp->t_atom;
    if (tp->type == TOK_XMLPI)
        pn->pn_atom2 = tp->t_atom2;
    return pn;
}

/*
 * Parse the productions:
 *
 *      XMLNameExpr:
 *              XMLName XMLNameExpr?
 *              { Expr } XMLNameExpr?
 *
 * Return a PN_LIST, PN_UNARY, or PN_NULLARY according as XMLNameExpr produces
 * a list of names and/or expressions, a single expression, or a single name.
 * If PN_LIST or PN_NULLARY, pn_type will be TOK_XMLNAME; if PN_UNARY, pn_type
 * will be TOK_LC.
 */
static JSParseNode *
XMLNameExpr(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc)
{
    JSParseNode *pn, *pn2, *list;
    JSTokenType tt;

    pn = list = NULL;
    do {
        tt = CURRENT_TOKEN(ts).type;
        if (tt == TOK_LC) {
            pn2 = XMLExpr(cx, ts, JS_TRUE, tc);
            if (!pn2)
                return NULL;
        } else {
            JS_ASSERT(tt == TOK_XMLNAME);
            pn2 = XMLAtomNode(cx, ts, tc);
            if (!pn2)
                return NULL;
        }

        if (!pn) {
            pn = pn2;
        } else {
            if (!list) {
                list = NewParseNode(PN_LIST, tc);
                if (!list)
                    return NULL;
                list->pn_type = TOK_XMLNAME;
                list->pn_pos.begin = pn->pn_pos.begin;
                list->initList(pn);
                list->pn_xflags = PNX_CANTFOLD;
                pn = list;
            }
            pn->pn_pos.end = pn2->pn_pos.end;
            pn->append(pn2);
        }
    } while ((tt = js_GetToken(cx, ts)) == TOK_XMLNAME || tt == TOK_LC);

    js_UngetToken(ts);
    return pn;
}

/*
 * Macro to test whether an XMLNameExpr or XMLTagContent node can be folded
 * at compile time into a JSXML tree.
 */
#define XML_FOLDABLE(pn)        ((pn)->pn_arity == PN_LIST                    \
                                 ? ((pn)->pn_xflags & PNX_CANTFOLD) == 0      \
                                 : (pn)->pn_type != TOK_LC)

/*
 * Parse the productions:
 *
 *      XMLTagContent:
 *              XMLNameExpr
 *              XMLTagContent S XMLNameExpr S? = S? XMLAttr
 *              XMLTagContent S XMLNameExpr S? = S? { Expr }
 *
 * Return a PN_LIST, PN_UNARY, or PN_NULLARY according to how XMLTagContent
 * produces a list of name and attribute values and/or braced expressions, a
 * single expression, or a single name.
 *
 * If PN_LIST or PN_NULLARY, pn_type will be TOK_XMLNAME for the case where
 * XMLTagContent: XMLNameExpr.  If pn_type is not TOK_XMLNAME but pn_arity is
 * PN_LIST, pn_type will be tagtype.  If PN_UNARY, pn_type will be TOK_LC and
 * we parsed exactly one expression.
 */
static JSParseNode *
XMLTagContent(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc,
              JSTokenType tagtype, JSAtom **namep)
{
    JSParseNode *pn, *pn2, *list;
    JSTokenType tt;

    pn = XMLNameExpr(cx, ts, tc);
    if (!pn)
        return NULL;
    *namep = (pn->pn_arity == PN_NULLARY) ? pn->pn_atom : NULL;
    list = NULL;

    while (js_MatchToken(cx, ts, TOK_XMLSPACE)) {
        tt = js_GetToken(cx, ts);
        if (tt != TOK_XMLNAME && tt != TOK_LC) {
            js_UngetToken(ts);
            break;
        }

        pn2 = XMLNameExpr(cx, ts, tc);
        if (!pn2)
            return NULL;
        if (!list) {
            list = NewParseNode(PN_LIST, tc);
            if (!list)
                return NULL;
            list->pn_type = tagtype;
            list->pn_pos.begin = pn->pn_pos.begin;
            list->initList(pn);
            pn = list;
        }
        pn->append(pn2);
        if (!XML_FOLDABLE(pn2))
            pn->pn_xflags |= PNX_CANTFOLD;

        js_MatchToken(cx, ts, TOK_XMLSPACE);
        MUST_MATCH_TOKEN(TOK_ASSIGN, JSMSG_NO_ASSIGN_IN_XML_ATTR);
        js_MatchToken(cx, ts, TOK_XMLSPACE);

        tt = js_GetToken(cx, ts);
        if (tt == TOK_XMLATTR) {
            pn2 = XMLAtomNode(cx, ts, tc);
        } else if (tt == TOK_LC) {
            pn2 = XMLExpr(cx, ts, JS_TRUE, tc);
            pn->pn_xflags |= PNX_CANTFOLD;
        } else {
            js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                        JSMSG_BAD_XML_ATTR_VALUE);
            return NULL;
        }
        if (!pn2)
            return NULL;
        pn->pn_pos.end = pn2->pn_pos.end;
        pn->append(pn2);
    }

    return pn;
}

#define XML_CHECK_FOR_ERROR_AND_EOF(tt,result)                                \
    JS_BEGIN_MACRO                                                            \
        if ((tt) <= TOK_EOF) {                                                \
            if ((tt) == TOK_EOF) {                                            \
                js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,     \
                                            JSMSG_END_OF_XML_SOURCE);         \
            }                                                                 \
            return result;                                                    \
        }                                                                     \
    JS_END_MACRO

static JSParseNode *
XMLElementOrList(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc,
                 JSBool allowList);

/*
 * Consume XML element tag content, including the TOK_XMLETAGO (</) sequence
 * that opens the end tag for the container.
 */
static JSBool
XMLElementContent(JSContext *cx, JSTokenStream *ts, JSParseNode *pn,
                  JSTreeContext *tc)
{
    JSTokenType tt;
    JSParseNode *pn2;
    JSAtom *textAtom;

    ts->flags &= ~TSF_XMLTAGMODE;
    for (;;) {
        ts->flags |= TSF_XMLTEXTMODE;
        tt = js_GetToken(cx, ts);
        ts->flags &= ~TSF_XMLTEXTMODE;
        XML_CHECK_FOR_ERROR_AND_EOF(tt, JS_FALSE);

        JS_ASSERT(tt == TOK_XMLSPACE || tt == TOK_XMLTEXT);
        textAtom = CURRENT_TOKEN(ts).t_atom;
        if (textAtom) {
            /* Non-zero-length XML text scanned. */
            pn2 = XMLAtomNode(cx, ts, tc);
            if (!pn2)
                return JS_FALSE;
            pn->pn_pos.end = pn2->pn_pos.end;
            pn->append(pn2);
        }

        ts->flags |= TSF_OPERAND;
        tt = js_GetToken(cx, ts);
        ts->flags &= ~TSF_OPERAND;
        XML_CHECK_FOR_ERROR_AND_EOF(tt, JS_FALSE);
        if (tt == TOK_XMLETAGO)
            break;

        if (tt == TOK_LC) {
            pn2 = XMLExpr(cx, ts, JS_FALSE, tc);
            pn->pn_xflags |= PNX_CANTFOLD;
        } else if (tt == TOK_XMLSTAGO) {
            pn2 = XMLElementOrList(cx, ts, tc, JS_FALSE);
            if (pn2) {
                pn2->pn_xflags &= ~PNX_XMLROOT;
                pn->pn_xflags |= pn2->pn_xflags;
            }
        } else {
            JS_ASSERT(tt == TOK_XMLCDATA || tt == TOK_XMLCOMMENT ||
                      tt == TOK_XMLPI);
            pn2 = XMLAtomNode(cx, ts, tc);
        }
        if (!pn2)
            return JS_FALSE;
        pn->pn_pos.end = pn2->pn_pos.end;
        pn->append(pn2);
    }

    JS_ASSERT(CURRENT_TOKEN(ts).type == TOK_XMLETAGO);
    ts->flags |= TSF_XMLTAGMODE;
    return JS_TRUE;
}

/*
 * Return a PN_LIST node containing an XML or XMLList Initialiser.
 */
static JSParseNode *
XMLElementOrList(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc,
                 JSBool allowList)
{
    JSParseNode *pn, *pn2, *list;
    JSTokenType tt;
    JSAtom *startAtom, *endAtom;

    JS_CHECK_RECURSION(cx, return NULL);

    JS_ASSERT(CURRENT_TOKEN(ts).type == TOK_XMLSTAGO);
    pn = NewParseNode(PN_LIST, tc);
    if (!pn)
        return NULL;

    ts->flags |= TSF_XMLTAGMODE;
    tt = js_GetToken(cx, ts);
    if (tt == TOK_ERROR)
        return NULL;

    if (tt == TOK_XMLNAME || tt == TOK_LC) {
        /*
         * XMLElement.  Append the tag and its contents, if any, to pn.
         */
        pn2 = XMLTagContent(cx, ts, tc, TOK_XMLSTAGO, &startAtom);
        if (!pn2)
            return NULL;
        js_MatchToken(cx, ts, TOK_XMLSPACE);

        tt = js_GetToken(cx, ts);
        if (tt == TOK_XMLPTAGC) {
            /* Point tag (/>): recycle pn if pn2 is a list of tag contents. */
            if (pn2->pn_type == TOK_XMLSTAGO) {
                pn->makeEmpty();
                RecycleTree(pn, tc);
                pn = pn2;
            } else {
                JS_ASSERT(pn2->pn_type == TOK_XMLNAME ||
                          pn2->pn_type == TOK_LC);
                pn->initList(pn2);
                if (!XML_FOLDABLE(pn2))
                    pn->pn_xflags |= PNX_CANTFOLD;
            }
            pn->pn_type = TOK_XMLPTAGC;
            pn->pn_xflags |= PNX_XMLROOT;
        } else {
            /* We had better have a tag-close (>) at this point. */
            if (tt != TOK_XMLTAGC) {
                js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                            JSMSG_BAD_XML_TAG_SYNTAX);
                return NULL;
            }
            pn2->pn_pos.end = CURRENT_TOKEN(ts).pos.end;

            /* Make sure pn2 is a TOK_XMLSTAGO list containing tag contents. */
            if (pn2->pn_type != TOK_XMLSTAGO) {
                pn->initList(pn2);
                if (!XML_FOLDABLE(pn2))
                    pn->pn_xflags |= PNX_CANTFOLD;
                pn2 = pn;
                pn = NewParseNode(PN_LIST, tc);
                if (!pn)
                    return NULL;
            }

            /* Now make pn a nominal-root TOK_XMLELEM list containing pn2. */
            pn->pn_type = TOK_XMLELEM;
            pn->pn_pos.begin = pn2->pn_pos.begin;
            pn->initList(pn2);
            if (!XML_FOLDABLE(pn2))
                pn->pn_xflags |= PNX_CANTFOLD;
            pn->pn_xflags |= PNX_XMLROOT;

            /* Get element contents and delimiting end-tag-open sequence. */
            if (!XMLElementContent(cx, ts, pn, tc))
                return NULL;

            tt = js_GetToken(cx, ts);
            XML_CHECK_FOR_ERROR_AND_EOF(tt, NULL);
            if (tt != TOK_XMLNAME && tt != TOK_LC) {
                js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                            JSMSG_BAD_XML_TAG_SYNTAX);
                return NULL;
            }

            /* Parse end tag; check mismatch at compile-time if we can. */
            pn2 = XMLTagContent(cx, ts, tc, TOK_XMLETAGO, &endAtom);
            if (!pn2)
                return NULL;
            if (pn2->pn_type == TOK_XMLETAGO) {
                /* Oops, end tag has attributes! */
                js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                            JSMSG_BAD_XML_TAG_SYNTAX);
                return NULL;
            }
            if (endAtom && startAtom && endAtom != startAtom) {
                JSString *str = ATOM_TO_STRING(startAtom);

                /* End vs. start tag name mismatch: point to the tag name. */
                js_ReportCompileErrorNumber(cx, ts, pn2,
                                            JSREPORT_UC | JSREPORT_ERROR,
                                            JSMSG_XML_TAG_NAME_MISMATCH,
                                            str->chars());
                return NULL;
            }

            /* Make a TOK_XMLETAGO list with pn2 as its single child. */
            JS_ASSERT(pn2->pn_type == TOK_XMLNAME || pn2->pn_type == TOK_LC);
            list = NewParseNode(PN_LIST, tc);
            if (!list)
                return NULL;
            list->pn_type = TOK_XMLETAGO;
            list->initList(pn2);
            pn->append(list);
            if (!XML_FOLDABLE(pn2)) {
                list->pn_xflags |= PNX_CANTFOLD;
                pn->pn_xflags |= PNX_CANTFOLD;
            }

            js_MatchToken(cx, ts, TOK_XMLSPACE);
            MUST_MATCH_TOKEN(TOK_XMLTAGC, JSMSG_BAD_XML_TAG_SYNTAX);
        }

        /* Set pn_op now that pn has been updated to its final value. */
        pn->pn_op = JSOP_TOXML;
    } else if (allowList && tt == TOK_XMLTAGC) {
        /* XMLList Initialiser. */
        pn->pn_type = TOK_XMLLIST;
        pn->pn_op = JSOP_TOXMLLIST;
        pn->makeEmpty();
        pn->pn_xflags |= PNX_XMLROOT;
        if (!XMLElementContent(cx, ts, pn, tc))
            return NULL;

        MUST_MATCH_TOKEN(TOK_XMLTAGC, JSMSG_BAD_XML_LIST_SYNTAX);
    } else {
        js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                    JSMSG_BAD_XML_NAME_SYNTAX);
        return NULL;
    }

    pn->pn_pos.end = CURRENT_TOKEN(ts).pos.end;
    ts->flags &= ~TSF_XMLTAGMODE;
    return pn;
}

static JSParseNode *
XMLElementOrListRoot(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc,
                     JSBool allowList)
{
    uint32 oldopts;
    JSParseNode *pn;

    /*
     * Force XML support to be enabled so that comments and CDATA literals
     * are recognized, instead of <! followed by -- starting an HTML comment
     * to end of line (used in script tags to hide content from old browsers
     * that don't recognize <script>).
     */
    oldopts = JS_SetOptions(cx, cx->options | JSOPTION_XML);
    pn = XMLElementOrList(cx, ts, tc, allowList);
    JS_SetOptions(cx, oldopts);
    return pn;
}

JSParseNode *
JSCompiler::parseXMLText(JSObject *chain, bool allowList)
{
    /*
     * Push a compiler frame if we have no frames, or if the top frame is a
     * lightweight function activation, or if its scope chain doesn't match
     * the one passed to us.
     */
    JSTreeContext tc(this);
    tc.scopeChain = chain;

    /* Set XML-only mode to turn off special treatment of {expr} in XML. */
    TS(this)->flags |= TSF_OPERAND | TSF_XMLONLYMODE;
    JSTokenType tt = js_GetToken(context, TS(this));
    TS(this)->flags &= ~TSF_OPERAND;

    JSParseNode *pn;
    if (tt != TOK_XMLSTAGO) {
        js_ReportCompileErrorNumber(context, TS(this), NULL, JSREPORT_ERROR,
                                    JSMSG_BAD_XML_MARKUP);
        pn = NULL;
    } else {
        pn = XMLElementOrListRoot(context, TS(this), &tc, allowList);
    }

    TS(this)->flags &= ~TSF_XMLONLYMODE;
    return pn;
}

#endif /* JS_HAS_XMLSUPPORT */

#if JS_HAS_BLOCK_SCOPE
/*
 * Check whether blockid is an active scoping statement in tc. This code is
 * necessary to qualify tc->decls.lookup() hits in PrimaryExpr's TOK_NAME case
 * (below) where the hits come from Scheme-ish let bindings in for loop heads
 * and let blocks and expressions (not let declarations).
 *
 * Unlike let declarations ("let as the new var"), which is a kind of letrec
 * due to hoisting, let in a for loop head, let block, or let expression acts
 * like Scheme's let: initializers are evaluated without the new let bindings
 * being in scope.
 *
 * Name binding analysis is eager with fixups, rather than multi-pass, and let
 * bindings push on the front of the tc->decls JSAtomList (either the singular
 * list or on a hash chain -- see JSAtomList::AddHow) in order to shadow outer
 * scope bindings of the same name.
 *
 * This simplifies binding lookup code at the price of a linear search here,
 * but only if code uses let (var predominates), and even then this function's
 * loop iterates more than once only in crazy cases.
 */
static inline bool
BlockIdInScope(uintN blockid, JSTreeContext *tc)
{
    if (blockid > tc->blockid())
        return false;
    for (JSStmtInfo *stmt = tc->topScopeStmt; stmt; stmt = stmt->downScope) {
        if (stmt->blockid == blockid)
            return true;
    }
    return false;
}
#endif

static JSParseNode *
PrimaryExpr(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc,
            JSTokenType tt, JSBool afterDot)
{
    JSParseNode *pn, *pn2, *pn3;
    JSOp op;

    JS_CHECK_RECURSION(cx, return NULL);

#if JS_HAS_GETTER_SETTER
    if (tt == TOK_NAME) {
        tt = CheckGetterOrSetter(cx, ts, TOK_FUNCTION);
        if (tt == TOK_ERROR)
            return NULL;
    }
#endif

    switch (tt) {
      case TOK_FUNCTION:
#if JS_HAS_XML_SUPPORT
        ts->flags |= TSF_KEYWORD_IS_NAME;
        if (js_MatchToken(cx, ts, TOK_DBLCOLON)) {
            ts->flags &= ~TSF_KEYWORD_IS_NAME;
            pn2 = NewParseNode(PN_NULLARY, tc);
            if (!pn2)
                return NULL;
            pn2->pn_type = TOK_FUNCTION;
            pn = QualifiedSuffix(cx, ts, pn2, tc);
            if (!pn)
                return NULL;
            break;
        }
        ts->flags &= ~TSF_KEYWORD_IS_NAME;
#endif
        pn = FunctionExpr(cx, ts, tc);
        if (!pn)
            return NULL;
        break;

      case TOK_LB:
      {
        JSBool matched;
        jsuint index;

        pn = NewParseNode(PN_LIST, tc);
        if (!pn)
            return NULL;
        pn->pn_type = TOK_RB;
        pn->pn_op = JSOP_NEWINIT;
        pn->makeEmpty();

#if JS_HAS_GENERATORS
        pn->pn_blockid = tc->blockidGen;
#endif

        ts->flags |= TSF_OPERAND;
        matched = js_MatchToken(cx, ts, TOK_RB);
        ts->flags &= ~TSF_OPERAND;
        if (!matched) {
            for (index = 0; ; index++) {
                if (index == JS_ARGS_LENGTH_MAX) {
                    js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                                JSMSG_ARRAY_INIT_TOO_BIG);
                    return NULL;
                }

                ts->flags |= TSF_OPERAND;
                tt = js_PeekToken(cx, ts);
                ts->flags &= ~TSF_OPERAND;
                if (tt == TOK_RB) {
                    pn->pn_xflags |= PNX_ENDCOMMA;
                    break;
                }

                if (tt == TOK_COMMA) {
                    /* So CURRENT_TOKEN gets TOK_COMMA and not TOK_LB. */
                    js_MatchToken(cx, ts, TOK_COMMA);
                    pn2 = NewParseNode(PN_NULLARY, tc);
                    pn->pn_xflags |= PNX_HOLEY;
                } else {
                    pn2 = AssignExpr(cx, ts, tc);
                }
                if (!pn2)
                    return NULL;
                pn->append(pn2);

                if (tt != TOK_COMMA) {
                    /* If we didn't already match TOK_COMMA in above case. */
                    if (!js_MatchToken(cx, ts, TOK_COMMA))
                        break;
                }
            }

#if JS_HAS_GENERATORS
            /*
             * At this point, (index == 0 && pn->pn_count != 0) implies one
             * element initialiser was parsed.
             *
             * An array comprehension of the form:
             *
             *   [i * j for (i in o) for (j in p) if (i != j)]
             *
             * translates to roughly the following let expression:
             *
             *   let (array = new Array, i, j) {
             *     for (i in o) let {
             *       for (j in p)
             *         if (i != j)
             *           array.push(i * j)
             *     }
             *     array
             *   }
             *
             * where array is a nameless block-local variable.  The "roughly"
             * means that an implementation may optimize away the array.push.
             * An array comprehension opens exactly one block scope, no matter
             * how many for heads it contains.
             *
             * Each let () {...} or for (let ...) ... compiles to:
             *
             *   JSOP_ENTERBLOCK <o> ... JSOP_LEAVEBLOCK <n>
             *
             * where <o> is a literal object representing the block scope,
             * with <n> properties, naming each var declared in the block.
             *
             * Each var declaration in a let-block binds a name in <o> at
             * compile time, and allocates a slot on the operand stack at
             * runtime via JSOP_ENTERBLOCK.  A block-local var is accessed
             * by the JSOP_GETLOCAL and JSOP_SETLOCAL ops, and iterated with
             * JSOP_FORLOCAL.  These ops all have an immediate operand, the
             * local slot's stack index from fp->spbase.
             *
             * The array comprehension iteration step, array.push(i * j) in
             * the example above, is done by <i * j>; JSOP_ARRAYCOMP <array>,
             * where <array> is the index of array's stack slot.
             */
            if (index == 0 &&
                pn->pn_count != 0 &&
                js_MatchToken(cx, ts, TOK_FOR)) {
                JSParseNode *pnexp, *pntop;

                /* Relabel pn as an array comprehension node. */
                pn->pn_type = TOK_ARRAYCOMP;

                /*
                 * Remove the comprehension expression from pn's linked list
                 * and save it via pnexp.  We'll re-install it underneath the
                 * ARRAYPUSH node after we parse the rest of the comprehension.
                 */
                pnexp = pn->last();
                JS_ASSERT(pn->pn_count == 1 || pn->pn_count == 2);
                pn->pn_tail = (--pn->pn_count == 1)
                              ? &pn->pn_head->pn_next
                              : &pn->pn_head;
                *pn->pn_tail = NULL;

                pntop = ComprehensionTail(pnexp, pn->pn_blockid, tc,
                                          TOK_ARRAYPUSH, JSOP_ARRAYPUSH);
                if (!pntop)
                    return NULL;
                pn->append(pntop);
            }
#endif /* JS_HAS_GENERATORS */

            MUST_MATCH_TOKEN(TOK_RB, JSMSG_BRACKET_AFTER_LIST);
        }
        pn->pn_pos.end = CURRENT_TOKEN(ts).pos.end;
        return pn;
      }

      case TOK_LC:
      {
        JSBool afterComma;
        JSParseNode *pnval;

        pn = NewParseNode(PN_LIST, tc);
        if (!pn)
            return NULL;
        pn->pn_type = TOK_RC;
        pn->pn_op = JSOP_NEWINIT;
        pn->makeEmpty();

        afterComma = JS_FALSE;
        for (;;) {
            ts->flags |= TSF_KEYWORD_IS_NAME;
            tt = js_GetToken(cx, ts);
            ts->flags &= ~TSF_KEYWORD_IS_NAME;
            switch (tt) {
              case TOK_NUMBER:
                pn3 = NewParseNode(PN_NULLARY, tc);
                if (!pn3)
                    return NULL;
                pn3->pn_dval = CURRENT_TOKEN(ts).t_dval;
                break;
              case TOK_NAME:
#if JS_HAS_GETTER_SETTER
                {
                    JSAtom *atom;

                    atom = CURRENT_TOKEN(ts).t_atom;
                    if (atom == cx->runtime->atomState.getAtom)
                        op = JSOP_GETTER;
                    else if (atom == cx->runtime->atomState.setAtom)
                        op = JSOP_SETTER;
                    else
                        goto property_name;

                    ts->flags |= TSF_KEYWORD_IS_NAME;
                    tt = js_GetToken(cx, ts);
                    ts->flags &= ~TSF_KEYWORD_IS_NAME;
                    if (tt != TOK_NAME) {
                        js_UngetToken(ts);
                        goto property_name;
                    }
                    pn3 = NewNameNode(cx, CURRENT_TOKEN(ts).t_atom, tc);
                    if (!pn3)
                        return NULL;

                    /* We have to fake a 'function' token here. */
                    CURRENT_TOKEN(ts).t_op = JSOP_NOP;
                    CURRENT_TOKEN(ts).type = TOK_FUNCTION;
                    pn2 = FunctionExpr(cx, ts, tc);
                    pn2 = NewBinary(TOK_COLON, op, pn3, pn2, tc);
                    goto skip;
                }
              property_name:
#endif
              case TOK_STRING:
                pn3 = NewParseNode(PN_NULLARY, tc);
                if (!pn3)
                    return NULL;
                pn3->pn_atom = CURRENT_TOKEN(ts).t_atom;
                break;
              case TOK_RC:
                goto end_obj_init;
              default:
                js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                            JSMSG_BAD_PROP_ID);
                return NULL;
            }

            tt = js_GetToken(cx, ts);
#if JS_HAS_GETTER_SETTER
            if (tt == TOK_NAME) {
                tt = CheckGetterOrSetter(cx, ts, TOK_COLON);
                if (tt == TOK_ERROR)
                    return NULL;
            }
#endif

            if (tt != TOK_COLON) {
#if JS_HAS_DESTRUCTURING_SHORTHAND
                if (tt != TOK_COMMA && tt != TOK_RC) {
#endif
                    js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                                JSMSG_COLON_AFTER_ID);
                    return NULL;
#if JS_HAS_DESTRUCTURING_SHORTHAND
                }

                /*
                 * Support, e.g., |var {x, y} = o| as destructuring shorthand
                 * for |var {x: x, y: y} = o|, per proposed JS2/ES4 for JS1.8.
                 */
                js_UngetToken(ts);
                pn->pn_xflags |= PNX_DESTRUCT;
                pnval = pn3;
                if (pnval->pn_type == TOK_NAME) {
                    pnval->pn_arity = PN_NAME;
                    InitNameNodeCommon(pnval, tc);
                }
                op = JSOP_NOP;
#endif
            } else {
                op = CURRENT_TOKEN(ts).t_op;
                pnval = AssignExpr(cx, ts, tc);
            }

            pn2 = NewBinary(TOK_COLON, op, pn3, pnval, tc);
#if JS_HAS_GETTER_SETTER
          skip:
#endif
            if (!pn2)
                return NULL;
            pn->append(pn2);

            tt = js_GetToken(cx, ts);
            if (tt == TOK_RC)
                goto end_obj_init;
            if (tt != TOK_COMMA) {
                js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                            JSMSG_CURLY_AFTER_LIST);
                return NULL;
            }
            afterComma = JS_TRUE;
        }

      end_obj_init:
        pn->pn_pos.end = CURRENT_TOKEN(ts).pos.end;
        return pn;
      }

#if JS_HAS_BLOCK_SCOPE
      case TOK_LET:
        pn = LetBlock(cx, ts, tc, JS_FALSE);
        if (!pn)
            return NULL;
        break;
#endif

#if JS_HAS_SHARP_VARS
      case TOK_DEFSHARP:
        pn = NewParseNode(PN_UNARY, tc);
        if (!pn)
            return NULL;
        pn->pn_num = (jsint) CURRENT_TOKEN(ts).t_dval;
        ts->flags |= TSF_OPERAND;
        tt = js_GetToken(cx, ts);
        ts->flags &= ~TSF_OPERAND;
        if (tt == TOK_USESHARP || tt == TOK_DEFSHARP ||
#if JS_HAS_XML_SUPPORT
            tt == TOK_STAR || tt == TOK_AT ||
            tt == TOK_XMLSTAGO /* XXXbe could be sharp? */ ||
#endif
            tt == TOK_STRING || tt == TOK_NUMBER || tt == TOK_PRIMARY) {
            js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                        JSMSG_BAD_SHARP_VAR_DEF);
            return NULL;
        }
        pn->pn_kid = PrimaryExpr(cx, ts, tc, tt, JS_FALSE);
        if (!pn->pn_kid)
            return NULL;
        if (!tc->ensureSharpSlots())
            return NULL;
        break;

      case TOK_USESHARP:
        /* Check for forward/dangling references at runtime, to allow eval. */
        pn = NewParseNode(PN_NULLARY, tc);
        if (!pn)
            return NULL;
        if (!tc->ensureSharpSlots())
            return NULL;
        pn->pn_num = (jsint) CURRENT_TOKEN(ts).t_dval;
        break;
#endif /* JS_HAS_SHARP_VARS */

      case TOK_LP:
      {
        JSBool genexp;

        pn = ParenExpr(cx, ts, tc, NULL, &genexp);
        if (!pn)
            return NULL;
        pn->pn_parens = true;
        if (!genexp)
            MUST_MATCH_TOKEN(TOK_RP, JSMSG_PAREN_IN_PAREN);
        break;
      }

#if JS_HAS_XML_SUPPORT
      case TOK_STAR:
        pn = QualifiedIdentifier(cx, ts, tc);
        if (!pn)
            return NULL;
        break;

      case TOK_AT:
        pn = AttributeIdentifier(cx, ts, tc);
        if (!pn)
            return NULL;
        break;

      case TOK_XMLSTAGO:
        pn = XMLElementOrListRoot(cx, ts, tc, JS_TRUE);
        if (!pn)
            return NULL;
        break;
#endif /* JS_HAS_XML_SUPPORT */

      case TOK_STRING:
#if JS_HAS_SHARP_VARS
        /* FALL THROUGH */
#endif

#if JS_HAS_XML_SUPPORT
      case TOK_XMLCDATA:
      case TOK_XMLCOMMENT:
      case TOK_XMLPI:
#endif
        pn = NewParseNode(PN_NULLARY, tc);
        if (!pn)
            return NULL;
        pn->pn_atom = CURRENT_TOKEN(ts).t_atom;
#if JS_HAS_XML_SUPPORT
        if (tt == TOK_XMLPI)
            pn->pn_atom2 = CURRENT_TOKEN(ts).t_atom2;
        else
#endif
            pn->pn_op = CURRENT_TOKEN(ts).t_op;
        break;

      case TOK_NAME:
        pn = NewNameNode(cx, CURRENT_TOKEN(ts).t_atom, tc);
        if (!pn)
            return NULL;
        JS_ASSERT(CURRENT_TOKEN(ts).t_op == JSOP_NAME);
        pn->pn_op = JSOP_NAME;

        if ((tc->flags & (TCF_IN_FUNCTION | TCF_FUN_PARAM_ARGUMENTS)) == TCF_IN_FUNCTION &&
            pn->pn_atom == cx->runtime->atomState.argumentsAtom) {
            /*
             * Flag arguments usage so we can avoid unsafe optimizations such
             * as formal parameter assignment analysis (because of the hated
             * feature whereby arguments alias formals). We do this even for
             * a reference of the form foo.arguments, which ancient code may
             * still use instead of arguments (more hate).
             */
            NoteArgumentsUse(tc);

            /*
             * Bind early to JSOP_ARGUMENTS to relieve later code from having
             * to do this work (new rule for the emitter to count on).
             */
            if (!afterDot && !(tc->flags & TCF_DECL_DESTRUCTURING) && !tc->inStatement(STMT_WITH)) {
                pn->pn_op = JSOP_ARGUMENTS;
                pn->pn_dflags |= PND_BOUND;
            }
        } else if ((!afterDot
#if JS_HAS_XML_SUPPORT
                    || js_PeekToken(cx, ts) == TOK_DBLCOLON
#endif
                   ) && !(tc->flags & TCF_DECL_DESTRUCTURING)) {
            JSStmtInfo *stmt = js_LexicalLookup(tc, pn->pn_atom, NULL);
            if (!stmt || stmt->type != STMT_WITH) {
                JSDefinition *dn;

                JSAtomListElement *ale = tc->decls.lookup(pn->pn_atom);
                if (ale) {
                    dn = ALE_DEFN(ale);
#if JS_HAS_BLOCK_SCOPE
                    /*
                     * Skip out-of-scope let bindings along an ALE list or hash
                     * chain. These can happen due to |let (x = x) x| block and
                     * expression bindings, where the x on the right of = comes
                     * from an outer scope. See bug 496532.
                     */
                    while (dn->isLet() && !BlockIdInScope(dn->pn_blockid, tc)) {
                        do {
                            ale = ALE_NEXT(ale);
                        } while (ale && ALE_ATOM(ale) != pn->pn_atom);
                        if (!ale)
                            break;
                        dn = ALE_DEFN(ale);
                    }
#endif
                }

                if (ale) {
                    dn = ALE_DEFN(ale);
                } else {
                    ale = tc->lexdeps.lookup(pn->pn_atom);
                    if (ale) {
                        dn = ALE_DEFN(ale);
                    } else {
                        /*
                         * No definition before this use in any lexical scope.
                         * Add a mapping in tc->lexdeps from pn->pn_atom to a
                         * new node for the forward-referenced definition. This
                         * placeholder definition node will be adopted when we
                         * parse the real defining declaration form, or left as
                         * a free variable definition if we never see the real
                         * definition.
                         */
                        ale = MakePlaceholder(pn, tc);
                        if (!ale)
                            return NULL;
                        dn = ALE_DEFN(ale);

                        /*
                         * In case this is a forward reference to a function,
                         * we pessimistically set PND_FUNARG if the next token
                         * is not a left parenthesis.
                         *
                         * If the definition eventually parsed into dn is not a
                         * function, this flag won't hurt, and if we do parse a
                         * function with pn's name, then the PND_FUNARG flag is
                         * necessary for safe cx->display-based optimization of
                         * the closure's static link.
                         */
                        JS_ASSERT(PN_TYPE(dn) == TOK_NAME);
                        JS_ASSERT(dn->pn_op == JSOP_NOP);
                        if (js_PeekToken(cx, ts) != TOK_LP)
                            dn->pn_dflags |= PND_FUNARG;
                    }
                }

                JS_ASSERT(dn->pn_defn);
                LinkUseToDef(pn, dn, tc);

                /* Here we handle the backward function reference case. */
                if (js_PeekToken(cx, ts) != TOK_LP)
                    dn->pn_dflags |= PND_FUNARG;

                pn->pn_dflags |= (dn->pn_dflags & PND_FUNARG);
            }
        }

#if JS_HAS_XML_SUPPORT
        if (js_MatchToken(cx, ts, TOK_DBLCOLON)) {
            if (afterDot) {
                JSString *str;

                /*
                 * Here PrimaryExpr is called after . or .. followed by a name
                 * followed by ::. This is the only case where a keyword after
                 * . or .. is not treated as a property name.
                 */
                str = ATOM_TO_STRING(pn->pn_atom);
                tt = js_CheckKeyword(str->chars(), str->length());
                if (tt == TOK_FUNCTION) {
                    pn->pn_arity = PN_NULLARY;
                    pn->pn_type = TOK_FUNCTION;
                } else if (tt != TOK_EOF) {
                    js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                                JSMSG_KEYWORD_NOT_NS);
                    return NULL;
                }
            }
            pn = QualifiedSuffix(cx, ts, pn, tc);
            if (!pn)
                return NULL;
        }
#endif
        break;

      case TOK_REGEXP:
      {
        JSObject *obj;

        pn = NewParseNode(PN_NULLARY, tc);
        if (!pn)
            return NULL;

        obj = js_NewRegExpObject(cx, ts,
                                 ts->tokenbuf.begin(),
                                 ts->tokenbuf.length(),
                                 CURRENT_TOKEN(ts).t_reflags);
        if (!obj)
            return NULL;
        if (!(tc->flags & TCF_COMPILE_N_GO)) {
            STOBJ_CLEAR_PARENT(obj);
            STOBJ_CLEAR_PROTO(obj);
        }

        pn->pn_objbox = tc->compiler->newObjectBox(obj);
        if (!pn->pn_objbox)
            return NULL;

        pn->pn_op = JSOP_REGEXP;
        break;
      }

      case TOK_NUMBER:
        pn = NewParseNode(PN_NULLARY, tc);
        if (!pn)
            return NULL;
        pn->pn_op = JSOP_DOUBLE;
        pn->pn_dval = CURRENT_TOKEN(ts).t_dval;
        break;

      case TOK_PRIMARY:
        pn = NewParseNode(PN_NULLARY, tc);
        if (!pn)
            return NULL;
        pn->pn_op = CURRENT_TOKEN(ts).t_op;
        break;

      case TOK_ERROR:
        /* The scanner or one of its subroutines reported the error. */
        return NULL;

      default:
        js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                    JSMSG_SYNTAX_ERROR);
        return NULL;
    }
    return pn;
}

static JSParseNode *
ParenExpr(JSContext *cx, JSTokenStream *ts, JSTreeContext *tc,
          JSParseNode *pn1, JSBool *genexp)
{
    JSTokenPtr begin;
    JSParseNode *pn;

    JS_ASSERT(CURRENT_TOKEN(ts).type == TOK_LP);
    begin = CURRENT_TOKEN(ts).pos.begin;

    if (genexp)
        *genexp = JS_FALSE;
    pn = BracketedExpr(cx, ts, tc);
    if (!pn)
        return NULL;

#if JS_HAS_GENERATOR_EXPRS
    if (js_MatchToken(cx, ts, TOK_FOR)) {
        if (pn->pn_type == TOK_YIELD && !pn->pn_parens) {
            js_ReportCompileErrorNumber(cx, ts, pn, JSREPORT_ERROR,
                                        JSMSG_BAD_GENERATOR_SYNTAX,
                                        js_yield_str);
            return NULL;
        }
        if (pn->pn_type == TOK_COMMA && !pn->pn_parens) {
            js_ReportCompileErrorNumber(cx, ts, pn->last(), JSREPORT_ERROR,
                                        JSMSG_BAD_GENERATOR_SYNTAX,
                                        js_generator_str);
            return NULL;
        }
        if (!pn1) {
            pn1 = NewParseNode(PN_UNARY, tc);
            if (!pn1)
                return NULL;
        }
        pn = GeneratorExpr(pn1, pn, tc);
        if (!pn)
            return NULL;
        pn->pn_pos.begin = begin;
        if (genexp) {
            if (js_GetToken(cx, ts) != TOK_RP) {
                js_ReportCompileErrorNumber(cx, ts, NULL, JSREPORT_ERROR,
                                            JSMSG_BAD_GENERATOR_SYNTAX,
                                            js_generator_str);
                return NULL;
            }
            pn->pn_pos.end = CURRENT_TOKEN(ts).pos.end;
            *genexp = JS_TRUE;
        }
    }
#endif /* JS_HAS_GENERATOR_EXPRS */

    return pn;
}

/*
 * Fold from one constant type to another.
 * XXX handles only strings and numbers for now
 */
static JSBool
FoldType(JSContext *cx, JSParseNode *pn, JSTokenType type)
{
    if (PN_TYPE(pn) != type) {
        switch (type) {
          case TOK_NUMBER:
            if (pn->pn_type == TOK_STRING) {
                jsdouble d;
                if (!JS_ValueToNumber(cx, ATOM_KEY(pn->pn_atom), &d))
                    return JS_FALSE;
                pn->pn_dval = d;
                pn->pn_type = TOK_NUMBER;
                pn->pn_op = JSOP_DOUBLE;
            }
            break;

          case TOK_STRING:
            if (pn->pn_type == TOK_NUMBER) {
                JSString *str = js_NumberToString(cx, pn->pn_dval);
                if (!str)
                    return JS_FALSE;
                pn->pn_atom = js_AtomizeString(cx, str, 0);
                if (!pn->pn_atom)
                    return JS_FALSE;
                pn->pn_type = TOK_STRING;
                pn->pn_op = JSOP_STRING;
            }
            break;

          default:;
        }
    }
    return JS_TRUE;
}

/*
 * Fold two numeric constants.  Beware that pn1 and pn2 are recycled, unless
 * one of them aliases pn, so you can't safely fetch pn2->pn_next, e.g., after
 * a successful call to this function.
 */
static JSBool
FoldBinaryNumeric(JSContext *cx, JSOp op, JSParseNode *pn1, JSParseNode *pn2,
                  JSParseNode *pn, JSTreeContext *tc)
{
    jsdouble d, d2;
    int32 i, j;

    JS_ASSERT(pn1->pn_type == TOK_NUMBER && pn2->pn_type == TOK_NUMBER);
    d = pn1->pn_dval;
    d2 = pn2->pn_dval;
    switch (op) {
      case JSOP_LSH:
      case JSOP_RSH:
        i = js_DoubleToECMAInt32(d);
        j = js_DoubleToECMAInt32(d2);
        j &= 31;
        d = (op == JSOP_LSH) ? i << j : i >> j;
        break;

      case JSOP_URSH:
        j = js_DoubleToECMAInt32(d2);
        j &= 31;
        d = js_DoubleToECMAUint32(d) >> j;
        break;

      case JSOP_ADD:
        d += d2;
        break;

      case JSOP_SUB:
        d -= d2;
        break;

      case JSOP_MUL:
        d *= d2;
        break;

      case JSOP_DIV:
        if (d2 == 0) {
#if defined(XP_WIN)
            /* XXX MSVC miscompiles such that (NaN == 0) */
            if (JSDOUBLE_IS_NaN(d2))
                d = *cx->runtime->jsNaN;
            else
#endif
            if (d == 0 || JSDOUBLE_IS_NaN(d))
                d = *cx->runtime->jsNaN;
            else if (JSDOUBLE_IS_NEG(d) != JSDOUBLE_IS_NEG(d2))
                d = *cx->runtime->jsNegativeInfinity;
            else
                d = *cx->runtime->jsPositiveInfinity;
        } else {
            d /= d2;
        }
        break;

      case JSOP_MOD:
        if (d2 == 0) {
            d = *cx->runtime->jsNaN;
        } else {
            d = js_fmod(d, d2);
        }
        break;

      default:;
    }

    /* Take care to allow pn1 or pn2 to alias pn. */
    if (pn1 != pn)
        RecycleTree(pn1, tc);
    if (pn2 != pn)
        RecycleTree(pn2, tc);
    pn->pn_type = TOK_NUMBER;
    pn->pn_op = JSOP_DOUBLE;
    pn->pn_arity = PN_NULLARY;
    pn->pn_dval = d;
    return JS_TRUE;
}

#if JS_HAS_XML_SUPPORT

static JSBool
FoldXMLConstants(JSContext *cx, JSParseNode *pn, JSTreeContext *tc)
{
    JSTokenType tt;
    JSParseNode **pnp, *pn1, *pn2;
    JSString *accum, *str;
    uint32 i, j;
    JSTempValueRooter tvr;

    JS_ASSERT(pn->pn_arity == PN_LIST);
    tt = PN_TYPE(pn);
    pnp = &pn->pn_head;
    pn1 = *pnp;
    accum = NULL;
    if ((pn->pn_xflags & PNX_CANTFOLD) == 0) {
        if (tt == TOK_XMLETAGO)
            accum = ATOM_TO_STRING(cx->runtime->atomState.etagoAtom);
        else if (tt == TOK_XMLSTAGO || tt == TOK_XMLPTAGC)
            accum = ATOM_TO_STRING(cx->runtime->atomState.stagoAtom);
    }

    /*
     * GC Rooting here is tricky: for most of the loop, |accum| is safe via
     * the newborn string root. However, when |pn2->pn_type| is TOK_XMLCDATA,
     * TOK_XMLCOMMENT, or TOK_XMLPI it is knocked out of the newborn root.
     * Therefore, we have to add additonal protection from GC nesting under
     * js_ConcatStrings.
     */
    for (pn2 = pn1, i = j = 0; pn2; pn2 = pn2->pn_next, i++) {
        /* The parser already rejected end-tags with attributes. */
        JS_ASSERT(tt != TOK_XMLETAGO || i == 0);
        switch (pn2->pn_type) {
          case TOK_XMLATTR:
            if (!accum)
                goto cantfold;
            /* FALL THROUGH */
          case TOK_XMLNAME:
          case TOK_XMLSPACE:
          case TOK_XMLTEXT:
          case TOK_STRING:
            if (pn2->pn_arity == PN_LIST)
                goto cantfold;
            str = ATOM_TO_STRING(pn2->pn_atom);
            break;

          case TOK_XMLCDATA:
            str = js_MakeXMLCDATAString(cx, ATOM_TO_STRING(pn2->pn_atom));
            if (!str)
                return JS_FALSE;
            break;

          case TOK_XMLCOMMENT:
            str = js_MakeXMLCommentString(cx, ATOM_TO_STRING(pn2->pn_atom));
            if (!str)
                return JS_FALSE;
            break;

          case TOK_XMLPI:
            str = js_MakeXMLPIString(cx, ATOM_TO_STRING(pn2->pn_atom),
                                         ATOM_TO_STRING(pn2->pn_atom2));
            if (!str)
                return JS_FALSE;
            break;

          cantfold:
          default:
            JS_ASSERT(*pnp == pn1);
            if ((tt == TOK_XMLSTAGO || tt == TOK_XMLPTAGC) &&
                (i & 1) ^ (j & 1)) {
#ifdef DEBUG_brendanXXX
                printf("1: %d, %d => ", i, j);
                if (accum)
                    js_FileEscapedString(stdout, accum, 0);
                else
                    fputs("NULL", stdout);
                fputc('\n', stdout);
#endif
            } else if (accum && pn1 != pn2) {
                while (pn1->pn_next != pn2) {
                    pn1 = RecycleTree(pn1, tc);
                    --pn->pn_count;
                }
                pn1->pn_type = TOK_XMLTEXT;
                pn1->pn_op = JSOP_STRING;
                pn1->pn_arity = PN_NULLARY;
                pn1->pn_atom = js_AtomizeString(cx, accum, 0);
                if (!pn1->pn_atom)
                    return JS_FALSE;
                JS_ASSERT(pnp != &pn1->pn_next);
                *pnp = pn1;
            }
            pnp = &pn2->pn_next;
            pn1 = *pnp;
            accum = NULL;
            continue;
        }

        if (accum) {
            JS_PUSH_TEMP_ROOT_STRING(cx, accum, &tvr);
            str = ((tt == TOK_XMLSTAGO || tt == TOK_XMLPTAGC) && i != 0)
                  ? js_AddAttributePart(cx, i & 1, accum, str)
                  : js_ConcatStrings(cx, accum, str);
            JS_POP_TEMP_ROOT(cx, &tvr);
            if (!str)
                return JS_FALSE;
#ifdef DEBUG_brendanXXX
            printf("2: %d, %d => ", i, j);
            js_FileEscapedString(stdout, str, 0);
            printf(" (%u)\n", str->length());
#endif
            ++j;
        }
        accum = str;
    }

    if (accum) {
        str = NULL;
        if ((pn->pn_xflags & PNX_CANTFOLD) == 0) {
            if (tt == TOK_XMLPTAGC)
                str = ATOM_TO_STRING(cx->runtime->atomState.ptagcAtom);
            else if (tt == TOK_XMLSTAGO || tt == TOK_XMLETAGO)
                str = ATOM_TO_STRING(cx->runtime->atomState.tagcAtom);
        }
        if (str) {
            accum = js_ConcatStrings(cx, accum, str);
            if (!accum)
                return JS_FALSE;
        }

        JS_ASSERT(*pnp == pn1);
        while (pn1->pn_next) {
            pn1 = RecycleTree(pn1, tc);
            --pn->pn_count;
        }
        pn1->pn_type = TOK_XMLTEXT;
        pn1->pn_op = JSOP_STRING;
        pn1->pn_arity = PN_NULLARY;
        pn1->pn_atom = js_AtomizeString(cx, accum, 0);
        if (!pn1->pn_atom)
            return JS_FALSE;
        JS_ASSERT(pnp != &pn1->pn_next);
        *pnp = pn1;
    }

    if (pn1 && pn->pn_count == 1) {
        /*
         * Only one node under pn, and it has been folded: move pn1 onto pn
         * unless pn is an XML root (in which case we need it to tell the code
         * generator to emit a JSOP_TOXML or JSOP_TOXMLLIST op).  If pn is an
         * XML root *and* it's a point-tag, rewrite it to TOK_XMLELEM to avoid
         * extra "<" and "/>" bracketing at runtime.
         */
        if (!(pn->pn_xflags & PNX_XMLROOT)) {
            pn->become(pn1);
        } else if (tt == TOK_XMLPTAGC) {
            pn->pn_type = TOK_XMLELEM;
            pn->pn_op = JSOP_TOXML;
        }
    }
    return JS_TRUE;
}

#endif /* JS_HAS_XML_SUPPORT */

static int
Boolish(JSParseNode *pn)
{
    switch (pn->pn_op) {
      case JSOP_DOUBLE:
        return pn->pn_dval != 0 && !JSDOUBLE_IS_NaN(pn->pn_dval);

      case JSOP_STRING:
        return ATOM_TO_STRING(pn->pn_atom)->length() != 0;

#if JS_HAS_GENERATOR_EXPRS
      case JSOP_CALL:
      {
        /*
         * A generator expression as an if or loop condition has no effects, it
         * simply results in a truthy object reference. This condition folding
         * is needed for the decompiler. See bug 442342 and bug 443074.
         */
        if (pn->pn_count != 1)
            break;
        JSParseNode *pn2 = pn->pn_head;
        if (pn2->pn_type != TOK_FUNCTION)
            break;
        if (!(pn2->pn_funbox->tcflags & TCF_GENEXP_LAMBDA))
            break;
        /* FALL THROUGH */
      }
#endif

      case JSOP_DEFFUN:
      case JSOP_LAMBDA:
      case JSOP_THIS:
      case JSOP_TRUE:
        return 1;

      case JSOP_NULL:
      case JSOP_FALSE:
        return 0;

      default:;
    }
    return -1;
}

JSBool
js_FoldConstants(JSContext *cx, JSParseNode *pn, JSTreeContext *tc, bool inCond)
{
    JSParseNode *pn1 = NULL, *pn2 = NULL, *pn3 = NULL;

    JS_CHECK_RECURSION(cx, return JS_FALSE);

    switch (pn->pn_arity) {
      case PN_FUNC:
      {
        uint32 oldflags = tc->flags;
        JSFunctionBox *oldlist = tc->functionList;

        tc->flags = pn->pn_funbox->tcflags;
        tc->functionList = pn->pn_funbox->kids;
        if (!js_FoldConstants(cx, pn->pn_body, tc))
            return JS_FALSE;
        pn->pn_funbox->kids = tc->functionList;
        tc->flags = oldflags;
        tc->functionList = oldlist;
        break;
      }

      case PN_LIST:
      {
        /* Propagate inCond through logical connectives. */
        bool cond = inCond && (pn->pn_type == TOK_OR || pn->pn_type == TOK_AND);

        /* Save the list head in pn1 for later use. */
        for (pn1 = pn2 = pn->pn_head; pn2; pn2 = pn2->pn_next) {
            if (!js_FoldConstants(cx, pn2, tc, cond))
                return JS_FALSE;
        }
        break;
      }

      case PN_TERNARY:
        /* Any kid may be null (e.g. for (;;)). */
        pn1 = pn->pn_kid1;
        pn2 = pn->pn_kid2;
        pn3 = pn->pn_kid3;
        if (pn1 && !js_FoldConstants(cx, pn1, tc, pn->pn_type == TOK_IF))
            return JS_FALSE;
        if (pn2) {
            if (!js_FoldConstants(cx, pn2, tc, pn->pn_type == TOK_FORHEAD))
                return JS_FALSE;
            if (pn->pn_type == TOK_FORHEAD && pn2->pn_op == JSOP_TRUE) {
                RecycleTree(pn2, tc);
                pn->pn_kid2 = NULL;
            }
        }
        if (pn3 && !js_FoldConstants(cx, pn3, tc))
            return JS_FALSE;
        break;

      case PN_BINARY:
        pn1 = pn->pn_left;
        pn2 = pn->pn_right;

        /* Propagate inCond through logical connectives. */
        if (pn->pn_type == TOK_OR || pn->pn_type == TOK_AND) {
            if (!js_FoldConstants(cx, pn1, tc, inCond))
                return JS_FALSE;
            if (!js_FoldConstants(cx, pn2, tc, inCond))
                return JS_FALSE;
            break;
        }

        /* First kid may be null (for default case in switch). */
        if (pn1 && !js_FoldConstants(cx, pn1, tc, pn->pn_type == TOK_WHILE))
            return JS_FALSE;
        if (!js_FoldConstants(cx, pn2, tc, pn->pn_type == TOK_DO))
            return JS_FALSE;
        break;

      case PN_UNARY:
        /* Our kid may be null (e.g. return; vs. return e;). */
        pn1 = pn->pn_kid;
        if (pn1 && !js_FoldConstants(cx, pn1, tc, pn->pn_op == JSOP_NOT))
            return JS_FALSE;
        break;

      case PN_NAME:
        /*
         * Skip pn1 down along a chain of dotted member expressions to avoid
         * excessive recursion.  Our only goal here is to fold constants (if
         * any) in the primary expression operand to the left of the first
         * dot in the chain.
         */
        if (!pn->pn_used) {
            pn1 = pn->pn_expr;
            while (pn1 && pn1->pn_arity == PN_NAME && !pn1->pn_used)
                pn1 = pn1->pn_expr;
            if (pn1 && !js_FoldConstants(cx, pn1, tc))
                return JS_FALSE;
        }
        break;

      case PN_NAMESET:
        pn1 = pn->pn_tree;
        if (!js_FoldConstants(cx, pn1, tc))
            return JS_FALSE;
        break;

      case PN_NULLARY:
        break;
    }

    switch (pn->pn_type) {
      case TOK_IF:
        if (ContainsStmt(pn2, TOK_VAR) || ContainsStmt(pn3, TOK_VAR))
            break;
        /* FALL THROUGH */

      case TOK_HOOK:
        /* Reduce 'if (C) T; else E' into T for true C, E for false. */
        switch (pn1->pn_type) {
          case TOK_NUMBER:
            if (pn1->pn_dval == 0 || JSDOUBLE_IS_NaN(pn1->pn_dval))
                pn2 = pn3;
            break;
          case TOK_STRING:
            if (ATOM_TO_STRING(pn1->pn_atom)->length() == 0)
                pn2 = pn3;
            break;
          case TOK_PRIMARY:
            if (pn1->pn_op == JSOP_TRUE)
                break;
            if (pn1->pn_op == JSOP_FALSE || pn1->pn_op == JSOP_NULL) {
                pn2 = pn3;
                break;
            }
            /* FALL THROUGH */
          default:
            /* Early return to dodge common code that copies pn2 to pn. */
            return JS_TRUE;
        }

#if JS_HAS_GENERATOR_EXPRS
        /* Don't fold a trailing |if (0)| in a generator expression. */
        if (!pn2 && (tc->flags & TCF_GENEXP_LAMBDA))
            break;
#endif

        if (pn2 && !pn2->pn_defn)
            pn->become(pn2);
        if (!pn2 || (pn->pn_type == TOK_SEMI && !pn->pn_kid)) {
            /*
             * False condition and no else, or an empty then-statement was
             * moved up over pn.  Either way, make pn an empty block (not an
             * empty statement, which does not decompile, even when labeled).
             * NB: pn must be a TOK_IF as TOK_HOOK can never have a null kid
             * or an empty statement for a child.
             */
            pn->pn_type = TOK_LC;
            pn->pn_arity = PN_LIST;
            pn->makeEmpty();
        }
        RecycleTree(pn2, tc);
        if (pn3 && pn3 != pn2)
            RecycleTree(pn3, tc);
        break;

      case TOK_OR:
      case TOK_AND:
        if (inCond) {
            if (pn->pn_arity == PN_LIST) {
                JSParseNode **pnp = &pn->pn_head;
                JS_ASSERT(*pnp == pn1);
                do {
                    int cond = Boolish(pn1);
                    if (cond == (pn->pn_type == TOK_OR)) {
                        for (pn2 = pn1->pn_next; pn2; pn2 = pn3) {
                            pn3 = pn2->pn_next;
                            RecycleTree(pn2, tc);
                            --pn->pn_count;
                        }
                        pn1->pn_next = NULL;
                        break;
                    }
                    if (cond != -1) {
                        JS_ASSERT(cond == (pn->pn_type == TOK_AND));
                        if (pn->pn_count == 1)
                            break;
                        *pnp = pn1->pn_next;
                        RecycleTree(pn1, tc);
                        --pn->pn_count;
                    } else {
                        pnp = &pn1->pn_next;
                    }
                } while ((pn1 = *pnp) != NULL);

                // We may have to change arity from LIST to BINARY.
                pn1 = pn->pn_head;
                if (pn->pn_count == 2) {
                    pn2 = pn1->pn_next;
                    pn1->pn_next = NULL;
                    JS_ASSERT(!pn2->pn_next);
                    pn->pn_arity = PN_BINARY;
                    pn->pn_left = pn1;
                    pn->pn_right = pn2;
                } else if (pn->pn_count == 1) {
                    pn->become(pn1);
                    RecycleTree(pn1, tc);
                }
            } else {
                int cond = Boolish(pn1);
                if (cond == (pn->pn_type == TOK_OR)) {
                    RecycleTree(pn2, tc);
                    pn->become(pn1);
                } else if (cond != -1) {
                    JS_ASSERT(cond == (pn->pn_type == TOK_AND));
                    RecycleTree(pn1, tc);
                    pn->become(pn2);
                }
            }
        }
        break;

      case TOK_ASSIGN:
        /*
         * Compound operators such as *= should be subject to folding, in case
         * the left-hand side is constant, and so that the decompiler produces
         * the same string that you get from decompiling a script or function
         * compiled from that same string.  As with +, += is special.
         */
        if (pn->pn_op == JSOP_NOP)
            break;
        if (pn->pn_op != JSOP_ADD)
            goto do_binary_op;
        /* FALL THROUGH */

      case TOK_PLUS:
        if (pn->pn_arity == PN_LIST) {
            size_t length, length2;
            jschar *chars;
            JSString *str, *str2;

            /*
             * Any string literal term with all others number or string means
             * this is a concatenation.  If any term is not a string or number
             * literal, we can't fold.
             */
            JS_ASSERT(pn->pn_count > 2);
            if (pn->pn_xflags & PNX_CANTFOLD)
                return JS_TRUE;
            if (pn->pn_xflags != PNX_STRCAT)
                goto do_binary_op;

            /* Ok, we're concatenating: convert non-string constant operands. */
            length = 0;
            for (pn2 = pn1; pn2; pn2 = pn2->pn_next) {
                if (!FoldType(cx, pn2, TOK_STRING))
                    return JS_FALSE;
                /* XXX fold only if all operands convert to string */
                if (pn2->pn_type != TOK_STRING)
                    return JS_TRUE;
                length += ATOM_TO_STRING(pn2->pn_atom)->flatLength();
            }

            /* Allocate a new buffer and string descriptor for the result. */
            chars = (jschar *) cx->malloc((length + 1) * sizeof(jschar));
            if (!chars)
                return JS_FALSE;
            str = js_NewString(cx, chars, length);
            if (!str) {
                cx->free(chars);
                return JS_FALSE;
            }

            /* Fill the buffer, advancing chars and recycling kids as we go. */
            for (pn2 = pn1; pn2; pn2 = RecycleTree(pn2, tc)) {
                str2 = ATOM_TO_STRING(pn2->pn_atom);
                length2 = str2->flatLength();
                js_strncpy(chars, str2->flatChars(), length2);
                chars += length2;
            }
            *chars = 0;

            /* Atomize the result string and mutate pn to refer to it. */
            pn->pn_atom = js_AtomizeString(cx, str, 0);
            if (!pn->pn_atom)
                return JS_FALSE;
            pn->pn_type = TOK_STRING;
            pn->pn_op = JSOP_STRING;
            pn->pn_arity = PN_NULLARY;
            break;
        }

        /* Handle a binary string concatenation. */
        JS_ASSERT(pn->pn_arity == PN_BINARY);
        if (pn1->pn_type == TOK_STRING || pn2->pn_type == TOK_STRING) {
            JSString *left, *right, *str;

            if (!FoldType(cx, (pn1->pn_type != TOK_STRING) ? pn1 : pn2,
                          TOK_STRING)) {
                return JS_FALSE;
            }
            if (pn1->pn_type != TOK_STRING || pn2->pn_type != TOK_STRING)
                return JS_TRUE;
            left = ATOM_TO_STRING(pn1->pn_atom);
            right = ATOM_TO_STRING(pn2->pn_atom);
            str = js_ConcatStrings(cx, left, right);
            if (!str)
                return JS_FALSE;
            pn->pn_atom = js_AtomizeString(cx, str, 0);
            if (!pn->pn_atom)
                return JS_FALSE;
            pn->pn_type = TOK_STRING;
            pn->pn_op = JSOP_STRING;
            pn->pn_arity = PN_NULLARY;
            RecycleTree(pn1, tc);
            RecycleTree(pn2, tc);
            break;
        }

        /* Can't concatenate string literals, let's try numbers. */
        goto do_binary_op;

      case TOK_STAR:
      case TOK_SHOP:
      case TOK_MINUS:
      case TOK_DIVOP:
      do_binary_op:
        if (pn->pn_arity == PN_LIST) {
            JS_ASSERT(pn->pn_count > 2);
            for (pn2 = pn1; pn2; pn2 = pn2->pn_next) {
                if (!FoldType(cx, pn2, TOK_NUMBER))
                    return JS_FALSE;
            }
            for (pn2 = pn1; pn2; pn2 = pn2->pn_next) {
                /* XXX fold only if all operands convert to number */
                if (pn2->pn_type != TOK_NUMBER)
                    break;
            }
            if (!pn2) {
                JSOp op = PN_OP(pn);

                pn2 = pn1->pn_next;
                pn3 = pn2->pn_next;
                if (!FoldBinaryNumeric(cx, op, pn1, pn2, pn, tc))
                    return JS_FALSE;
                while ((pn2 = pn3) != NULL) {
                    pn3 = pn2->pn_next;
                    if (!FoldBinaryNumeric(cx, op, pn, pn2, pn, tc))
                        return JS_FALSE;
                }
            }
        } else {
            JS_ASSERT(pn->pn_arity == PN_BINARY);
            if (!FoldType(cx, pn1, TOK_NUMBER) ||
                !FoldType(cx, pn2, TOK_NUMBER)) {
                return JS_FALSE;
            }
            if (pn1->pn_type == TOK_NUMBER && pn2->pn_type == TOK_NUMBER) {
                if (!FoldBinaryNumeric(cx, PN_OP(pn), pn1, pn2, pn, tc))
                    return JS_FALSE;
            }
        }
        break;

      case TOK_UNARYOP:
        if (pn1->pn_type == TOK_NUMBER) {
            jsdouble d;

            /* Operate on one numeric constant. */
            d = pn1->pn_dval;
            switch (pn->pn_op) {
              case JSOP_BITNOT:
                d = ~js_DoubleToECMAInt32(d);
                break;

              case JSOP_NEG:
                d = -d;
                break;

              case JSOP_POS:
                break;

              case JSOP_NOT:
                pn->pn_type = TOK_PRIMARY;
                pn->pn_op = (d == 0 || JSDOUBLE_IS_NaN(d)) ? JSOP_TRUE : JSOP_FALSE;
                pn->pn_arity = PN_NULLARY;
                /* FALL THROUGH */

              default:
                /* Return early to dodge the common TOK_NUMBER code. */
                return JS_TRUE;
            }
            pn->pn_type = TOK_NUMBER;
            pn->pn_op = JSOP_DOUBLE;
            pn->pn_arity = PN_NULLARY;
            pn->pn_dval = d;
            RecycleTree(pn1, tc);
        } else if (pn1->pn_type == TOK_PRIMARY) {
            if (pn->pn_op == JSOP_NOT &&
                (pn1->pn_op == JSOP_TRUE ||
                 pn1->pn_op == JSOP_FALSE)) {
                pn->become(pn1);
                pn->pn_op = (pn->pn_op == JSOP_TRUE) ? JSOP_FALSE : JSOP_TRUE;
                RecycleTree(pn1, tc);
            }
        }
        break;

#if JS_HAS_XML_SUPPORT
      case TOK_XMLELEM:
      case TOK_XMLLIST:
      case TOK_XMLPTAGC:
      case TOK_XMLSTAGO:
      case TOK_XMLETAGO:
      case TOK_XMLNAME:
        if (pn->pn_arity == PN_LIST) {
            JS_ASSERT(pn->pn_type == TOK_XMLLIST || pn->pn_count != 0);
            if (!FoldXMLConstants(cx, pn, tc))
                return JS_FALSE;
        }
        break;

      case TOK_AT:
        if (pn1->pn_type == TOK_XMLNAME) {
            jsval v;
            JSObjectBox *xmlbox;

            v = ATOM_KEY(pn1->pn_atom);
            if (!js_ToAttributeName(cx, &v))
                return JS_FALSE;
            JS_ASSERT(!JSVAL_IS_PRIMITIVE(v));

            xmlbox = tc->compiler->newObjectBox(JSVAL_TO_OBJECT(v));
            if (!xmlbox)
                return JS_FALSE;

            pn->pn_type = TOK_XMLNAME;
            pn->pn_op = JSOP_OBJECT;
            pn->pn_arity = PN_NULLARY;
            pn->pn_objbox = xmlbox;
            RecycleTree(pn1, tc);
        }
        break;
#endif /* JS_HAS_XML_SUPPORT */

      default:;
    }

    if (inCond) {
        int cond = Boolish(pn);
        if (cond >= 0) {
            switch (pn->pn_arity) {
              case PN_LIST:
                pn2 = pn->pn_head;
                do {
                    pn3 = pn2->pn_next;
                    RecycleTree(pn2, tc);
                } while ((pn2 = pn3) != NULL);
                break;
              case PN_FUNC:
                RecycleFuncNameKids(pn, tc);
                break;
              case PN_NULLARY:
                break;
              default:
                JS_NOT_REACHED("unhandled arity");
            }
            pn->pn_type = TOK_PRIMARY;
            pn->pn_op = cond ? JSOP_TRUE : JSOP_FALSE;
            pn->pn_arity = PN_NULLARY;
        }
    }

    return JS_TRUE;
}
