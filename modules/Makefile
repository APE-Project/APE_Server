CFLAGS=-Wall -g -shared -fPIC -rdynamic
CC=gcc -std=c99 -D_GNU_SOURCE

all: modules

modules: libape-chat.c libape-mysql.c libape-setcord.c libape-helloworld.c

	$(CC) $(CFLAGS) -Wl,-soname,libmod_spidermonkey.so -o lib/libmod_spidermonkey.so libape-spidermonkey.c -I../libs/js1.8/src/dist/include/ -L../libs/js1.8/src/dist/lib/ -lmozjs -Xlinker -rpath -Xlinker ../libs/js1.8/src/dist/lib/
	#$(CC) -Wall -g -bundle -undefined suppress -flat_namespace -o lib/libmod_spidermonkey.so libape-spidermonkey.c -I../libs/js1.8/src/dist/include/ -L../libs/js1.8/src/dist/lib/ -lmozjs -Xlinker -rpath -Xlinker ../libs/js1.8/src/dist/lib/
