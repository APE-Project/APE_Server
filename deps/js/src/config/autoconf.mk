#
# ***** BEGIN LICENSE BLOCK *****
# Version: MPL 1.1/GPL 2.0/LGPL 2.1
#
# The contents of this file are subject to the Mozilla Public License Version
# 1.1 (the "License"); you may not use this file except in compliance with
# the License. You may obtain a copy of the License at
# http://www.mozilla.org/MPL/
#
# Software distributed under the License is distributed on an "AS IS" basis,
# WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
# for the specific language governing rights and limitations under the
# License.
#
# The Original Code is this file as it was released upon August 6, 1998.
#
# The Initial Developer of the Original Code is
# Christopher Seawood.
# Portions created by the Initial Developer are Copyright (C) 1998
# the Initial Developer. All Rights Reserved.
#
# Contributor(s):
#   Benjamin Smedberg <benjamin@smedbergs.us>
#
# Alternatively, the contents of this file may be used under the terms of
# either of the GNU General Public License Version 2 or later (the "GPL"),
# or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
# in which case the provisions of the GPL or the LGPL are applicable instead
# of those above. If you wish to allow use of your version of this file only
# under the terms of either the GPL or the LGPL, and not to allow others to
# use your version of this file under the terms of the MPL, indicate your
# decision by deleting the provisions above and replace them with the notice
# and other provisions required by the GPL or the LGPL. If you do not delete
# the provisions above, a recipient may use your version of this file under
# the terms of any one of the MPL, the GPL or the LGPL.
#
# ***** END LICENSE BLOCK *****

# A netscape style .mk file for autoconf builds

INCLUDED_AUTOCONF_MK = 1
USE_AUTOCONF 	= 1
MOZILLA_CLIENT	= 1
target          = x86_64-apple-darwin10.2.0
ac_configure_args = 
BUILD_MODULES	= @BUILD_MODULES@
MOZILLA_VERSION = 

MOZ_BUILD_APP = @MOZ_BUILD_APP@
MOZ_APP_NAME	= 
MOZ_APP_DISPLAYNAME = 
MOZ_APP_VERSION = 

MOZ_PKG_SPECIAL = 

prefix		= /usr/local
exec_prefix	= ${prefix}
bindir		= ${exec_prefix}/bin
includedir	= ${prefix}/include
libdir		= ${exec_prefix}/lib
datadir		= ${prefix}/share
mandir		= ${prefix}/man

installdir	= $(libdir)/$(MOZ_APP_NAME)-$(MOZ_APP_VERSION)
sdkdir		= $(libdir)/$(MOZ_APP_NAME)-devel-$(MOZ_APP_VERSION)

TOP_DIST	= dist
ifneq (,$(filter /%,$(TOP_DIST)))
DIST		= $(TOP_DIST)
else
DIST		= $(DEPTH)/$(TOP_DIST)
endif

MOZ_JS_LIBS		   = -L$(libdir) -lmozjs

MOZ_SYNC_BUILD_FILES = 

MOZ_DEBUG	= 
MOZ_DEBUG_MODULES = 
MOZ_PROFILE_MODULES = @MOZ_PROFILE_MODULES@
MOZ_DEBUG_ENABLE_DEFS		= -DDEBUG -D_DEBUG -DDEBUG_root -DTRACING
MOZ_DEBUG_DISABLE_DEFS	= -DNDEBUG -DTRIMMED
MOZ_DEBUG_FLAGS	= -g
MOZ_DEBUG_LDFLAGS= -framework ExceptionHandling
MOZ_DBGRINFO_MODULES	= 
MOZ_EXTENSIONS  = @MOZ_EXTENSIONS@
MOZ_IMG_DECODERS= @MOZ_IMG_DECODERS@
MOZ_IMG_ENCODERS= @MOZ_IMG_ENCODERS@
MOZ_JSDEBUGGER  = @MOZ_JSDEBUGGER@
MOZ_LEAKY	= 
MOZ_MEMORY      = 
MOZ_JPROF       = 
MOZ_SHARK       = 
MOZ_CALLGRIND   = 
MOZ_VTUNE       = 
DEHYDRA_PATH    = 

NS_TRACE_MALLOC = 
INCREMENTAL_LINKER = 
MACOSX_DEPLOYMENT_TARGET = 10.4
BUILD_STATIC_LIBS = 
ENABLE_TESTS	= 1
JS_ULTRASPARC_OPTS = @JS_ULTRASPARC_OPTS@

TAR=@TAR@

RM = rm -f

# The MOZ_UI_LOCALE var is used to build a particular locale. Do *not*
# use the var to change any binary files. Do *not* use this var unless you
# write rules for the "clean-locale" and "locale" targets.
MOZ_UI_LOCALE = 

MOZ_COMPONENTS_VERSION_SCRIPT_LDFLAGS = -Wl,-exported_symbols_list -Wl,$(BUILD_TOOLS)/gnu-ld-scripts/components-export-list
MOZ_COMPONENT_NSPR_LIBS=-L$(LIBXUL_DIST)/bin $(NSPR_LIBS)

MOZ_FIX_LINK_PATHS=-Wl,-executable_path,$(LIBXUL_DIST)/bin

XPCOM_FROZEN_LDOPTS=@XPCOM_FROZEN_LDOPTS@
XPCOM_LIBS=@XPCOM_LIBS@
MOZ_TIMELINE=

ENABLE_STRIP	= 
PKG_SKIP_STRIP	= 

MOZ_POST_DSO_LIB_COMMAND = 
MOZ_POST_PROGRAM_COMMAND = 

MOZ_BUILD_ROOT             = /Users/para/dev/ape_1.0/APE_Server/deps/js/src

MOZ_INSURE = 
MOZ_INSURIFYING = 
MOZ_INSURE_DIRS = 
MOZ_INSURE_EXCLUDE_DIRS = 

MOZ_NATIVE_NSPR = 

CROSS_COMPILE   = 

OS_CPPFLAGS	= 
OS_CFLAGS	= $(OS_CPPFLAGS) -Wall -W -Wno-unused -Wpointer-arith -Wcast-align -W -Wno-long-long -fno-strict-aliasing -fpascal-strings -fno-common -pthread -pipe
OS_CXXFLAGS	= $(OS_CPPFLAGS) -fno-rtti -fno-exceptions -Wall -Wpointer-arith -Woverloaded-virtual -Wsynth -Wno-ctor-dtor-privacy -Wno-non-virtual-dtor -Wcast-align -Wno-invalid-offsetof -Wno-variadic-macros -Wno-long-long -fno-strict-aliasing -fpascal-strings -fno-common -pthread -pipe
OS_LDFLAGS	=   -framework Cocoa

OS_COMPILE_CFLAGS = $(OS_CPPFLAGS) -include $(DEPTH)/js-confdefs.h -DMOZILLA_CLIENT $(filter-out %/.pp,-Wp,-MD,$(MDDEPDIR)/$(basename $(@F)).pp)
OS_COMPILE_CXXFLAGS = $(OS_CPPFLAGS) -DMOZILLA_CLIENT -include $(DEPTH)/js-confdefs.h $(filter-out %/.pp,-Wp,-MD,$(MDDEPDIR)/$(basename $(@F)).pp)

OS_INCLUDES	= $(NSPR_CFLAGS)
OS_LIBS		= 
ACDEFINES	=  -DX_DISPLAY_MISSING=1 -DHAVE_64BIT_OS=1 -DD_INO=d_ino -DFEATURE_NANOJIT=1 -DJS_TRACER=1 -DAVMPLUS_AMD64=1 -DAVMPLUS_64BIT=1 -DAVMPLUS_UNIX=1 -DSTDC_HEADERS=1 -DHAVE_ST_BLKSIZE=1 -DHAVE_SIGINFO_T=1 -DJS_HAVE_STDINT_H=1 -DJS_BYTES_PER_WORD=8 -DJS_BITS_PER_WORD_LOG2=6 -DJS_ALIGN_OF_POINTER=8 -DJS_BYTES_PER_DOUBLE=8 -DHAVE_INT16_T=1 -DHAVE_INT32_T=1 -DHAVE_INT64_T=1 -DHAVE_UINT=1 -DHAVE_VISIBILITY_HIDDEN_ATTRIBUTE=1 -DHAVE_VISIBILITY_ATTRIBUTE=1 -DHAVE_DIRENT_H=1 -DHAVE_GETOPT_H=1 -DHAVE_MEMORY_H=1 -DHAVE_UNISTD_H=1 -DHAVE_NL_TYPES_H=1 -DHAVE_X11_XKBLIB_H=1 -DHAVE_SYS_STATVFS_H=1 -DHAVE_SYS_MOUNT_H=1 -DHAVE_MMINTRIN_H=1 -DNEW_H=\<new\> -DHAVE_SYS_CDEFS_H=1 -DHAVE_DLADDR=1 -DHAVE_FCHMOD=1 -DHAVE_FLOCKFILE=1 -DHAVE_GETC_UNLOCKED=1 -DHAVE_GETPAGESIZE=1 -DHAVE_LCHOWN=1 -DHAVE_LOCALTIME_R=1 -DHAVE_LSTAT64=1 -DHAVE_MEMMOVE=1 -DHAVE_RANDOM=1 -DHAVE_RINT=1 -DHAVE_SBRK=1 -DHAVE_SNPRINTF=1 -DHAVE_STAT64=1 -DHAVE_STATVFS=1 -DHAVE_STRERROR=1 -DHAVE_STRTOK_R=1 -DHAVE_WCRTOMB=1 -DHAVE_MBRTOWC=1 -DVA_COPY=va_copy -DHAVE_VA_COPY=1 -DHAVE_VA_LIST_AS_ARRAY=1 -DHAVE_CPP_EXPLICIT=1 -DHAVE_CPP_TYPENAME=1 -DHAVE_CPP_MODERN_SPECIALIZE_TEMPLATE_SYNTAX=1 -DHAVE_CPP_PARTIAL_SPECIALIZATION=1 -DHAVE_CPP_ACCESS_CHANGING_USING=1 -DHAVE_CPP_AMBIGUITY_RESOLVING_USING=1 -DHAVE_CPP_NAMESPACE_STD=1 -DHAVE_CPP_UNAMBIGUOUS_STD_NOTEQUAL=1 -DHAVE_CPP_NEW_CASTS=1 -DHAVE_CPP_DYNAMIC_CAST_TO_VOID_PTR=1 -DNEED_CPP_UNUSED_IMPLEMENTATIONS=1 -DHAVE_I18N_LC_MESSAGES=1 -DHAVE___CXA_DEMANGLE=1 -DHAVE__UNWIND_BACKTRACE=1 -DHAVE_TM_ZONE_TM_GMTOFF=1 -DCPP_THROW_NEW=throw\(\) -DEDITLINE=1 -DMOZ_DLL_SUFFIX=\".dylib\" -DXP_UNIX=1 -DUNIX_ASYNC_DNS=1 -DHAVE_SETLOCALE=1 

WARNINGS_AS_ERRORS = -Werror

MOZ_OPTIMIZE	= 1
MOZ_OPTIMIZE_FLAGS = -O3
MOZ_OPTIMIZE_LDFLAGS = -Wl,-dead_strip
MOZ_OPTIMIZE_SIZE_TWEAK = 

MOZ_RTTI_FLAGS_ON = -frtti
MOZ_EXCEPTIONS_FLAGS_ON = -fexceptions

MOZ_PROFILE_GUIDED_OPTIMIZE_DISABLE = 
PROFILE_GEN_CFLAGS = -fprofile-generate
PROFILE_GEN_LDFLAGS = -fprofile-generate
PROFILE_USE_CFLAGS = -fprofile-use
PROFILE_USE_LDFLAGS = -fprofile-use

WIN_TOP_SRC	= 
CYGWIN_WRAPPER	= 
AS_PERL         = 
CYGDRIVE_MOUNT	= 
AR		= ar
AR_FLAGS	= cr $@
AR_EXTRACT	= $(AR) x
AR_LIST		= $(AR) t
AR_DELETE	= $(AR) d
AS		= $(CC)
ASFLAGS		=  -fPIC
AS_DASH_C_FLAG	= -c
LD		= ld
RC		= 
RCFLAGS		= 
WINDRES		= :
IMPLIB		= 
FILTER		= 
BIN_FLAGS	= 
_MSC_VER	= 

DLL_PREFIX	= lib
LIB_PREFIX	= lib
OBJ_SUFFIX	= o
LIB_SUFFIX	= a
DLL_SUFFIX	= .dylib
BIN_SUFFIX	= 
ASM_SUFFIX	= s
IMPORT_LIB_SUFFIX = 
USE_N32		= 
HAVE_64BIT_OS	= 1

# Temp hack.  It is not my intention to leave this crap in here for ever.
# Im talking to fur right now to solve the problem without introducing 
# NS_USE_NATIVE to the build system -ramiro.
NS_USE_NATIVE = 

CC		    = gcc-4.2
CXX		    = g++-4.2

CC_VERSION	= gcc version 4.2.1 (Apple Inc. build 5646) (dot 1)
CXX_VERSION	= gcc version 4.2.1 (Apple Inc. build 5646) (dot 1)

GNU_AS		= 1
GNU_LD		= 
GNU_CC		= 1
GNU_CXX		= 1
HAVE_GCC3_ABI	= 1
INTEL_CC	= 
INTEL_CXX	= 

HOST_CC		= gcc-4.2
HOST_CXX	= g++-4.2
HOST_CFLAGS	=  -DXP_UNIX -DXP_MACOSX -DNO_X11
HOST_CXXFLAGS	= 
HOST_OPTIMIZE_FLAGS = -O3
HOST_NSPR_MDCPUCFG = \"md/_darwin.cfg\"
HOST_AR		= $(AR)
HOST_AR_FLAGS	= $(AR_FLAGS)
HOST_LD		= 
HOST_RANLIB	= ranlib
HOST_BIN_SUFFIX	= 

HOST_OS_ARCH	= darwin10.2.0
host_cpu	= x86_64
host_vendor	= apple
host_os		= darwin10.2.0

TARGET_NSPR_MDCPUCFG = \"md/_darwin.cfg\"
TARGET_CPU	= x86_64
TARGET_VENDOR	= apple
TARGET_OS	= darwin10.2.0
TARGET_MD_ARCH	= unix
TARGET_XPCOM_ABI = x86_64-gcc3

AUTOCONF	= /sw/bin/autoconf
PERL		= /opt/local/bin/perl5
PYTHON		= /usr/bin/python
RANLIB		= ranlib
WHOAMI		= /usr/bin/whoami
UNZIP		= @UNZIP@
ZIP		= @ZIP@
XARGS		= /usr/bin/xargs
STRIP		= strip -x -S
DOXYGEN		= :
PBBUILD_BIN	= /usr/bin/xcodebuild
SDP		= /usr/bin/sdp
NSINSTALL_BIN	= 

NSPR_CONFIG	= 
NSPR_CFLAGS	= 
NSPR_LIBS	= 

USE_DEPENDENT_LIBS = 1

JS_NATIVE_EDITLINE = 1
JS_DISABLE_SHELL   = 
EDITLINE_LIBS      = $(DEPTH)/editline/$(LIB_PREFIX)editline.$(LIB_SUFFIX)

# MKSHLIB_FORCE_ALL is used to force the linker to include all object
# files present in an archive. MKSHLIB_UNFORCE_ALL reverts the linker
# to normal behavior. Makefile's that create shared libraries out of
# archives use these flags to force in all of the .o files in the
# archives into the shared library.
WRAP_MALLOC_LIB         = 
WRAP_MALLOC_CFLAGS      = 
DSO_CFLAGS              = 
DSO_PIC_CFLAGS          = -fPIC
MKSHLIB                 = $(CXX) $(CXXFLAGS) $(DSO_PIC_CFLAGS) $(DSO_LDOPTS) -o $@
MKCSHLIB                = $(CC) $(CFLAGS) $(DSO_PIC_CFLAGS) $(DSO_LDOPTS) -o $@
MKSHLIB_FORCE_ALL       = 
MKSHLIB_UNFORCE_ALL     = 
DSO_LDOPTS              = 
DLL_SUFFIX              = .dylib

NO_LD_ARCHIVE_FLAGS     = 1

MOZ_TOOLKIT_REGISTRY_CFLAGS = \
	$(TK_CFLAGS)

MOZ_NATIVE_MAKEDEPEND	= /usr/X11/bin/makedepend

MOZ_AUTO_DEPS	= 1
COMPILER_DEPEND = 1
MDDEPDIR        := .deps

MOZ_DEMANGLE_SYMBOLS = 

# XXX - these need to be cleaned up and have real checks added -cls
CM_BLDTYPE=dbg
AWT_11=1
MOZ_BITS=32
OS_TARGET=Darwin
OS_ARCH=Darwin
OS_RELEASE=10.2.0
OS_TEST=x86_64

# For Solaris build
SOLARIS_SUNPRO_CC = 
SOLARIS_SUNPRO_CXX = 

# For AIX build
AIX_OBJMODEL = 

# For OS/2 build
MOZ_OS2_TOOLS = 
MOZ_OS2_USE_DECLSPEC = 
MOZ_OS2_HIGH_MEMORY = 1

MOZILLA_OFFICIAL = 

# Win32 options
MOZ_BROWSE_INFO	= 
MOZ_TOOLS_DIR	= 
MOZ_DEBUG_SYMBOLS = 
MOZ_QUANTIFY	= 
MSMANIFEST_TOOL = 
WIN32_REDIST_DIR = 
MOZ_MEMORY_LDFLAGS = 

# Codesighs tools option, enables win32 mapfiles.
MOZ_MAPINFO	= 

WINCE		= 
WINCE_WINDOWS_MOBILE = 

MACOS_SDK_DIR	= 
NEXT_ROOT	= 
GCC_VERSION	= 4.2
XCODEBUILD_VERSION= 1614
HAS_XCODE_2_1	= 1
UNIVERSAL_BINARY= 
HAVE_DTRACE= 

VISIBILITY_FLAGS = -fvisibility=hidden
WRAP_SYSTEM_INCLUDES = 

ENABLE_JIT = 1
NANOJIT_ARCH = X64
HAVE_ARM_SIMD= @HAVE_ARM_SIMD@
