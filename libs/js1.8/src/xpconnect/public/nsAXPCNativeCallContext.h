/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
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
 *   Nate Nielsen <nielsen@memberwebs.com> 
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

#ifndef nsAXPCNativeCallContext_h__
#define nsAXPCNativeCallContext_h__

class nsIXPConnectWrappedNative;

/**
* A native call context is allocated on the stack when XPConnect calls a
* native method. Holding a pointer to this object beyond the currently
* executing stack frame is not permitted.
*/
class nsAXPCNativeCallContext
{
public:
    NS_IMETHOD GetCallee(nsISupports **aResult) = 0;
    NS_IMETHOD GetCalleeMethodIndex(PRUint16 *aResult) = 0;
    NS_IMETHOD GetCalleeWrapper(nsIXPConnectWrappedNative **aResult) = 0;
    NS_IMETHOD GetJSContext(JSContext **aResult) = 0;
    NS_IMETHOD GetArgc(PRUint32 *aResult) = 0;
    NS_IMETHOD GetArgvPtr(jsval **aResult) = 0;

    /**
     * This may be NULL if the JS caller is ignoring the result of the call.
     */
    NS_IMETHOD GetRetValPtr(jsval **aResult) = 0;

    /**
     * Set this to indicate that the callee has directly set the return value
     * (using RetValPtr and the JSAPI). If set then xpconnect will not attempt
     * to overwrite it with the converted retval from the C++ callee.
     */
    NS_IMETHOD GetReturnValueWasSet(PRBool *aResult) = 0;
    NS_IMETHOD SetReturnValueWasSet(PRBool aValue) = 0;

    // Methods added since mozilla 0.6....

    NS_IMETHOD GetCalleeInterface(nsIInterfaceInfo **aResult) = 0;
    NS_IMETHOD GetCalleeClassInfo(nsIClassInfo **aResult) = 0;

    NS_IMETHOD GetPreviousCallContext(nsAXPCNativeCallContext **aResult) = 0;

    enum { LANG_UNKNOWN = 0,
           LANG_JS      = 1,
           LANG_NATIVE  = 2 };

    NS_IMETHOD GetLanguage(PRUint16 *aResult) = 0;
};

#endif
