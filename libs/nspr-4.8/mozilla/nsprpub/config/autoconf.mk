# -*- Mode: Makefile -*-

INCLUDED_AUTOCONF_MK = 1
USE_AUTOCONF	= 1

MOZILLA_CLIENT	= 

prefix		= /usr/local
exec_prefix	= ${prefix}
bindir		= ${exec_prefix}/bin
includedir	= ${prefix}/include/nspr
libdir		= ${exec_prefix}/lib
datadir		= ${prefix}/share

dist_prefix	= ${MOD_DEPTH}/dist
dist_bindir	= ${dist_prefix}/bin
dist_includedir = ${dist_prefix}/include/nspr
dist_libdir	= ${dist_prefix}/lib

DIST		= $(dist_prefix)

RELEASE_OBJDIR_NAME = Darwin10.0.0_DBG.OBJ
OBJDIR_NAME	= .
OBJDIR		= $(OBJDIR_NAME)
OBJ_SUFFIX	= o
LIB_SUFFIX	= a
DLL_SUFFIX	= dylib
ASM_SUFFIX	= s
MOD_NAME	= nspr20

MOD_MAJOR_VERSION = 4
MOD_MINOR_VERSION = 8
MOD_PATCH_VERSION = 0

LIBNSPR		= -L$(dist_libdir) -lnspr$(MOD_MAJOR_VERSION)
LIBPLC		= -L$(dist_libdir) -lplc$(MOD_MAJOR_VERSION)

CROSS_COMPILE	= 
BUILD_OPT	= 

USE_CPLUS	= 
USE_IPV6	= 
USE_N32		= 
USE_64		= 
GC_LEAK_DETECTOR = 
ENABLE_STRIP	= 

USE_PTHREADS	= 1
USE_BTHREADS	= 
PTHREADS_USER	= 
CLASSIC_NSPR	= 

AS		= $(CC) -x assembler-with-cpp
ASFLAGS		= $(CFLAGS)
CC		= gcc
CCC		= 
NS_USE_GCC	= 1
GCC_USE_GNU_LD	= 
MSC_VER		= 
AR		= /usr/bin/ar
AR_FLAGS	= cr $@
LD		= /usr/bin/ld
RANLIB		= ranlib
PERL		= /usr/bin/perl
RC		= 
RCFLAGS		= 
STRIP		= /usr/bin/strip -x -S
NSINSTALL	= $(MOD_DEPTH)/config/$(OBJDIR_NAME)/nsinstall
FILTER		= 
IMPLIB		= 
CYGWIN_WRAPPER	= 
MT		= 

OS_CPPFLAGS	= 
OS_CFLAGS	= $(OS_CPPFLAGS)  -Wall -fno-common -pthread -g $(DSO_CFLAGS)
OS_CXXFLAGS	= $(OS_CPPFLAGS)  -pthread -g $(DSO_CFLAGS)
OS_LIBS         =  
OS_LDFLAGS	= 
OS_DLLFLAGS	= 
DLLFLAGS	= 
EXEFLAGS  = 
OPTIMIZER	= 

MKSHLIB		= $(CC) $(DSO_LDOPTS) -o $@
DSO_CFLAGS	= -fPIC
DSO_LDOPTS	= -dynamiclib -compatibility_version 1 -current_version 1 -all_load -install_name @executable_path/$@ -headerpad_max_install_names

RESOLVE_LINK_SYMBOLS = 

HOST_CC		= gcc
HOST_CFLAGS	=  -DXP_UNIX
HOST_LDFLAGS	= 

DEFINES		=  -UNDEBUG -DDEBUG_root  -DDEBUG=1 -DXP_UNIX=1 -DDARWIN=1 -DHAVE_BSD_FLOCK=1 -DHAVE_SOCKLEN_T=1 -DXP_MACOSX=1 -DHAVE_LCHOWN=1 -DHAVE_STRERROR=1 

MDCPUCFG_H	= _darwin.cfg
PR_MD_CSRCS	= darwin.c
PR_MD_ASFILES	= os_Darwin.s
PR_MD_ARCH_DIR	= unix
CPU_ARCH	= i386

OS_TARGET	= MacOSX
OS_ARCH		= Darwin
OS_RELEASE	= 10.0.0
OS_TEST		= i386

NOSUCHFILE	= /no-such-file
AIX_LINK_OPTS	= 
MOZ_OBJFORMAT	= 
ULTRASPARC_LIBRARY = 

OBJECT_MODE	= 
ifdef OBJECT_MODE
export OBJECT_MODE
endif

VISIBILITY_FLAGS = 
WRAP_SYSTEM_INCLUDES = 

MACOSX_DEPLOYMENT_TARGET = 10.4
ifdef MACOSX_DEPLOYMENT_TARGET
export MACOSX_DEPLOYMENT_TARGET
endif

MACOS_SDK_DIR	= 

SYMBIAN_SDK_DIR = 

NEXT_ROOT	= 
ifdef NEXT_ROOT
export NEXT_ROOT
endif
