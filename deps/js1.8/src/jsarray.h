/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
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

#ifndef jsarray_h___
#define jsarray_h___
/*
 * JS Array interface.
 */
#include "jsprvtd.h"
#include "jspubtd.h"
#include "jsobj.h"

JS_BEGIN_EXTERN_C

#define ARRAY_CAPACITY_MIN      7

extern JSBool
js_IdIsIndex(jsval id, jsuint *indexp);

extern JSClass js_ArrayClass, js_SlowArrayClass;

static JS_INLINE JSBool
js_IsDenseArray(JSObject *obj)
{
    return STOBJ_GET_CLASS(obj) == &js_ArrayClass;
}

#define OBJ_IS_DENSE_ARRAY(cx, obj) js_IsDenseArray(obj)

#define OBJ_IS_ARRAY(cx,obj)    (OBJ_IS_DENSE_ARRAY(cx, obj) ||               \
                                 OBJ_GET_CLASS(cx, obj) == &js_SlowArrayClass)

/*
 * Dense arrays are not native (OBJ_IS_NATIVE(cx, aobj) for a dense array aobj
 * results in false, meaning aobj->map does not point to a JSScope).
 *
 * But Array methods are called via aobj.sort(), e.g., and the interpreter and
 * the trace recorder must consult the property cache in order to perform well.
 * The cache works only for native objects.
 *
 * Therefore the interpreter (js_Interpret in JSOP_GETPROP and JSOP_CALLPROP)
 * and js_GetPropertyHelper use this inline function to skip up one link in the
 * prototype chain when obj is a dense array, in order to find a native object
 * (to wit, Array.prototype) in which to probe for cached methods.
 *
 * Note that setting aobj.__proto__ for a dense array aobj turns aobj into a
 * slow array, avoiding the neede to skip.
 *
 * Callers of js_GetProtoIfDenseArray must take care to use the original object
 * (obj) for the |this| value of a getter, setter, or method call (bug 476447).
 */
static JS_INLINE JSObject *
js_GetProtoIfDenseArray(JSContext *cx, JSObject *obj)
{
    return OBJ_IS_DENSE_ARRAY(cx, obj) ? OBJ_GET_PROTO(cx, obj) : obj;
}

extern JSObject *
js_InitArrayClass(JSContext *cx, JSObject *obj);

extern bool
js_InitContextBusyArrayTable(JSContext *cx);

/*
 * Creates a new array with the given length and proto (NB: NULL is not
 * translated to Array.prototype), with len slots preallocated.
 */
extern JSObject * JS_FASTCALL
js_NewArrayWithSlots(JSContext* cx, JSObject* proto, uint32 len);

extern JSObject *
js_NewArrayObject(JSContext *cx, jsuint length, jsval *vector,
                  JSBool holey = JS_FALSE);

/* Create an array object that starts out already made slow/sparse. */
extern JSObject *
js_NewSlowArrayObject(JSContext *cx);

extern JSBool
js_MakeArraySlow(JSContext *cx, JSObject *obj);

#define JSSLOT_ARRAY_LENGTH            JSSLOT_PRIVATE
#define JSSLOT_ARRAY_COUNT             (JSSLOT_ARRAY_LENGTH + 1)
#define JSSLOT_ARRAY_UNUSED            (JSSLOT_ARRAY_COUNT + 1)

static JS_INLINE uint32
js_DenseArrayCapacity(JSObject *obj)
{
    JS_ASSERT(js_IsDenseArray(obj));
    return DSLOTS_IS_NOT_NULL(obj) ? (uint32) obj->dslots[-1] : 0;
}

static JS_INLINE void
js_SetDenseArrayCapacity(JSObject *obj, uint32 capacity)
{
    JS_ASSERT(js_IsDenseArray(obj));
    JS_ASSERT(DSLOTS_IS_NOT_NULL(obj));
    obj->dslots[-1] = (jsval) capacity;
}

extern JSBool
js_GetLengthProperty(JSContext *cx, JSObject *obj, jsuint *lengthp);

extern JSBool
js_SetLengthProperty(JSContext *cx, JSObject *obj, jsdouble length);

extern JSBool
js_HasLengthProperty(JSContext *cx, JSObject *obj, jsuint *lengthp);

extern JSBool JS_FASTCALL
js_IndexToId(JSContext *cx, jsuint index, jsid *idp);

/*
 * Test whether an object is "array-like".  Currently this means whether obj
 * is an Array or an arguments object.  We would like an API, and probably a
 * way in the language, to bless other objects as array-like: having indexed
 * properties, and a 'length' property of uint32 value equal to one more than
 * the greatest index.
 */
extern JSBool
js_IsArrayLike(JSContext *cx, JSObject *obj, JSBool *answerp, jsuint *lengthp);

/*
 * JS-specific merge sort function.
 */
typedef JSBool (*JSComparator)(void *arg, const void *a, const void *b,
                               int *result);
/*
 * NB: vec is the array to be sorted, tmp is temporary space at least as big
 * as vec. Both should be GC-rooted if appropriate.
 *
 * The sorted result is in vec. vec may be in an inconsistent state if the
 * comparator function cmp returns an error inside a comparison, so remember
 * to check the return value of this function.
 */
extern JSBool
js_MergeSort(void *vec, size_t nel, size_t elsize, JSComparator cmp,
             void *arg, void *tmp);

#ifdef DEBUG_ARRAYS
extern JSBool
js_ArrayInfo(JSContext *cx, JSObject *obj, uintN argc, jsval *argv, jsval *rval);
#endif

extern JSBool JS_FASTCALL
js_ArrayCompPush(JSContext *cx, JSObject *obj, jsval v);

/*
 * Fast dense-array-to-buffer conversion for use by canvas.
 *
 * If the array is a dense array, fill [offset..offset+count] values into
 * destination, assuming that types are consistent.  Return JS_TRUE if
 * successful, otherwise JS_FALSE -- note that the destination buffer may be
 * modified even if JS_FALSE is returned (e.g. due to finding an inappropriate
 * type later on in the array).  If JS_FALSE is returned, no error conditions
 * or exceptions are set on the context.
 *
 * This method succeeds if each element of the array is an integer or a double.
 * Values outside the 0-255 range are clamped to that range.  Double values are
 * converted to integers in this range by clamping and then rounding to
 * nearest, ties to even.
 */

JS_FRIEND_API(JSBool)
js_CoerceArrayToCanvasImageData(JSObject *obj, jsuint offset, jsuint count,
                                JSUint8 *dest);

JSBool
js_PrototypeHasIndexedProperties(JSContext *cx, JSObject *obj);

/*
 * Utility to access the value from the id returned by array_lookupProperty.
 */
JSBool
js_GetDenseArrayElementValue(JSContext *cx, JSObject *obj, JSProperty *prop,
                             jsval *vp);

/* Array constructor native. Exposed only so the JIT can know its address. */
JSBool
js_Array(JSContext* cx, JSObject* obj, uintN argc, jsval* argv, jsval* rval);

JS_END_EXTERN_C

#endif /* jsarray_h___ */
