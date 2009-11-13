/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
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
 *   Robert Ginda <rginda@netscape.com>
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

#include "nsCOMPtr.h"
#include "mozIJSSubScriptLoader.h"
#include "nsIScriptSecurityManager.h"

#define MOZ_JSSUBSCRIPTLOADER_CID                    \
{ /* 829814d6-1dd2-11b2-8e08-82fa0a339b00 */         \
    0x929814d6,                                      \
    0x1dd2,                                          \
    0x11b2,                                          \
    {0x8e, 0x08, 0x82, 0xfa, 0x0a, 0x33, 0x9b, 0x00} \
}

class mozJSSubScriptLoader : public mozIJSSubScriptLoader
{
public:
    mozJSSubScriptLoader();
    virtual ~mozJSSubScriptLoader();

    // all the interface method declarations...
    NS_DECL_ISUPPORTS
    NS_DECL_MOZIJSSUBSCRIPTLOADER

private:
    nsCOMPtr<nsIPrincipal> mSystemPrincipal;
    
};
