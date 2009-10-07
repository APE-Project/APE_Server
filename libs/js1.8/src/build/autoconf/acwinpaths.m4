dnl ***** BEGIN LICENSE BLOCK *****
dnl Version: MPL 1.1/GPL 2.0/LGPL 2.1
dnl
dnl The contents of this file are subject to the Mozilla Public License Version
dnl 1.1 (the "License"); you may not use this file except in compliance with
dnl the License. You may obtain a copy of the License at
dnl http://www.mozilla.org/MPL/
dnl
dnl Software distributed under the License is distributed on an "AS IS" basis,
dnl WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
dnl for the specific language governing rights and limitations under the
dnl License.
dnl
dnl The Original Code is mozilla.org code.
dnl
dnl The Initial Developer of the Original Code is the
dnl Mozilla Foundation <http://www.mozilla.org>
dnl
dnl Portions created by the Initial Developer are Copyright (C) 2009
dnl the Initial Developer. All Rights Reserved.
dnl
dnl Contributor(s):
dnl   Benjamin Smedberg
dnl
dnl Alternatively, the contents of this file may be used under the terms of
dnl either of the GNU General Public License Version 2 or later (the "GPL"),
dnl or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
dnl in which case the provisions of the GPL or the LGPL are applicable instead
dnl of those above. If you wish to allow use of your version of this file only
dnl under the terms of either the GPL or the LGPL, and not to allow others to
dnl use your version of this file under the terms of the MPL, indicate your
dnl decision by deleting the provisions above and replace them with the notice
dnl and other provisions required by the GPL or the LGPL. If you do not delete
dnl the provisions above, a recipient may use your version of this file under
dnl the terms of any one of the MPL, the GPL or the LGPL.
dnl
dnl ***** END LICENSE BLOCK *****

define(GENERATE_SUB_ABS, [
define([AC_OUTPUT_FILES_SUB1], [
patsubst($@, [/\*)], [/* | ?:/*)])
])
])
GENERATE_SUB_ABS(defn([AC_OUTPUT_FILES]))

define(GENERATE_SUB_NOSPLIT, [
define([AC_OUTPUT_FILES], [
patsubst($@, [-e "s%:% \$ac_given_srcdir/%g"], [])
])
])
GENERATE_SUB_NOSPLIT(defn([AC_OUTPUT_FILES_SUB1]))

define(GENERATE_HEADER_NOSPLIT, [
define([AC_OUTPUT_HEADER], [
patsubst($@, [-e "s%:% \$ac_given_srcdir/%g"], [])
])
])
GENERATE_HEADER_NOSPLIT(defn([AC_OUTPUT_HEADER]))

define(GENERATE_SUBDIRS_ABS, [
define([AC_OUTPUT_SUBDIRS], [
patsubst($@, [/\*)], [/* | ?:/*)])
])
])
GENERATE_SUBDIRS_ABS(defn([AC_OUTPUT_SUBDIRS]))
