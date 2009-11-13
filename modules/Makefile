include ./plateform.mk

CC=gcc -std=c99 -D_GNU_SOURCE

ifdef DARWIN_BUILD

all: modules

modules: libape-spidermonkey.c

	$(CC) -Wall -g -bundle -undefined suppress -flat_namespace -o lib/libmod_spidermonkey.so libape-spidermonkey.c -I../deps/js1.8/src/dist/include/ -L../deps/js1.8/src/ -ljs_static -lstdc++

endif
ifdef LINUX_BUILD

CFLAGS=-Wall -g -shared -fPIC -rdynamic

all: modules

modules: libape-spidermonkey.c

	$(CC) $(CFLAGS) -Wl,-soname,libmod_spidermonkey.so -o lib/libmod_spidermonkey.so libape-spidermonkey.c -I../deps/js1.8/src/dist/include/ -L../deps/js1.8/src/ -ljs_static -lstdc++
	
endif
