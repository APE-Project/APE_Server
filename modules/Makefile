CFLAGS=-Wall -g -shared -fPIC -rdynamic
CC=gcc -std=c99 -D_GNU_SOURCE

all: modules

modules: libape-chat.c libape-mysql.c libape-setcord.c libape-helloworld.c

	#$(CC) -Wall -g -bundle -undefined suppress -flat_namespace -o lib/libmod_spidermonkey.so libape-spidermonkey.c -I../libs/nspr-4.8/mozilla/nsprpub/dist/include/nspr/ -I../libs/js/src/ -I../libs/js/src/Darwin_OPT.OBJ -L../libs/js/src/Darwin_OPT.OBJ/ -ljs
	$(CC) -Wall -g -bundle -undefined suppress -flat_namespace -o lib/libmod_spidermonkey.so libape-spidermonkey.c -I/usr/local/include/js/ -lmozjs


