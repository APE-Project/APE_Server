include ./plateform.mk

CC=gcc -D_GNU_SOURCE

ifdef DARWIN_BUILD

all: modules

modules: libape-spidermonkey.c

	$(CC) -Wall -g -bundle -undefined suppress -flat_namespace -o lib/libmod_spidermonkey.so libape-spidermonkey.c -I../deps/js/src/dist/include/ -L../deps/js/src/ -ljs_static -lstdc++ -L./deps/mysac/ -I./deps/mysac/ -lmysac -L/opt/local/lib/mysql5/mysql/ -lmysqlclient_r

endif
ifdef LINUX_BUILD

CFLAGS=-Wall -g -shared -fPIC -rdynamic

all: modules

modules: libape-spidermonkey.c

	$(CC) $(CFLAGS) -Wl,-soname,libmod_spidermonkey.so -o lib/libmod_spidermonkey.so libape-spidermonkey.c -I../deps/js/src/dist/include/ -L../deps/js/src/ -ljs_static -lstdc++ -L./deps/mysac/ -I./deps/mysac/ -lmysac -lmysqlclient_r
	
endif
