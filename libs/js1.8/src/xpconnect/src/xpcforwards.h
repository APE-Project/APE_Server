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
 *   John Bandhauer <jband@netscape.com> (original author)
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

/* Private forward declarations. */

#ifndef xpcforwards_h___
#define xpcforwards_h___

// forward declarations of interally used classes...

class nsXPConnect;
class XPCJSRuntime;
class XPCContext;
class XPCCallContext;

class XPCPerThreadData;
class XPCJSThrower;
class XPCJSStack;

class nsXPCWrappedJS;
class nsXPCWrappedJSClass;

class XPCNativeMember;
class XPCNativeInterface;
class XPCNativeSet;

class XPCWrappedNative;
class XPCWrappedNativeProto;
class XPCWrappedNativeTearOff;
class XPCNativeScriptableShared;
class XPCNativeScriptableInfo;
class XPCNativeScriptableCreateInfo;

class XPCTraceableVariant;
class XPCJSObjectHolder;

class JSObject2WrappedJSMap;
class Native2WrappedNativeMap;
class IID2WrappedJSClassMap;
class IID2NativeInterfaceMap;
class ClassInfo2NativeSetMap;
class ClassInfo2WrappedNativeProtoMap;
class NativeSetMap;
class IID2ThisTranslatorMap;
class XPCNativeScriptableSharedMap;
class XPCWrappedNativeProtoMap;
class XPCNativeWrapperMap;
class WrappedNative2WrapperMap;

class nsXPCComponents;
class nsXPCComponents_Interfaces;
class nsXPCComponents_InterfacesByID;
class nsXPCComponents_Classes;
class nsXPCComponents_ClassesByID;
class nsXPCComponents_Results;
class nsXPCComponents_ID;
class nsXPCComponents_Exception;
class nsXPCComponents_Constructor;
class nsXPCComponents_Utils;
class nsXPCConstructor;

class AutoMarkingPtr;

class xpcProperty;

#ifdef XPC_IDISPATCH_SUPPORT
class XPCDispInterface;
struct IDispatch;
class XPCDispParams;
class XPCDispJSPropertyInfo;
class nsIXPConnectWrappedJS;
class XPCIDispatchExtension;
#endif


#endif /* xpcforwards_h___ */
