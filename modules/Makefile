include ./plateform.mk
include ./mysql.mk

CC=gcc -D_GNU_SOURCE

ifeq ($(HAS_MYSQL), yes)
	MYSQL_FLAGS = -L./deps/mysac/ -I./deps/mysac/ -lmysac -lmysqlclient_r
endif

ifdef DARWIN_BUILD

all: modules

modules: libape-spidermonkey.c

	$(CC) -Wall -O2 -bundle -undefined suppress -flat_namespace -o lib/libmod_spidermonkey.so libape-spidermonkey.c -I../deps/js/src/dist/include/ -L../deps/js/src/ -ljs_static -lstdc++ -L./deps/mysac/ -I./deps/mysac/ -lmysac -L/opt/local/lib/mysql5/mysql/ -lmysqlclient_r $(MYSQL_FLAGS)

endif
ifdef LINUX_BUILD

CFLAGS=-Wall -O2 -shared -fPIC -rdynamic

all: modules

modules: libape-spidermonkey.c

	$(CC) $(CFLAGS) -Wl,-soname,libmod_spidermonkey.so -o lib/libmod_spidermonkey.so libape-spidermonkey.c -I../deps/js/src/dist/include/ -L../deps/js/src/ -ljs_static -lstdc++ $(MYSQL_FLAGS)
	
endif
