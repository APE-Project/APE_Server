EXEC=bin/aped

prefix		= /usr/local
bindir		= $(prefix)/bin


SRC=src/entry.c src/sock.c src/hash.c src/handle_http.c src/cmd.c src/users.c src/channel.c src/config.c src/json.c src/plugins.c src/http.c src/extend.c src/utils.c src/ticks.c src/proxy.c src/base64.c src/pipe.c

CFLAGS=-Wall -O2 -minline-all-stringops -rdynamic 
LFLAGS=-ldl
CC=gcc
RM=rm -f

all: aped

aped: $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(EXEC) $(LFLAGS)
install: 
	install -d $(bindir)
	install -m 755 $(EXEC) $(bindir)

uninstall:
	$(RM) $(bindir)/aced

clean:
	$(RM) $(EXEC)
