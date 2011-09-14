EXEC=bin/aped

prefix		= /opt/APE_Server
bindir		= $(prefix)/bin


SRC=src/entry.c src/sock.c src/hash.c src/handle_http.c src/cmd.c src/users.c src/channel.c src/config.c src/json.c src/json_parser.c src/plugins.c src/http.c src/extend.c src/utils.c src/ticks.c src/base64.c src/pipe.c src/raw.c src/events.c src/event_kqueue.c src/event_epoll.c src/event_select.c src/transports.c src/servers.c src/dns.c src/sha1.c src/log.c src/parser.c src/md5.c

CFLAGS = -Wall -g -minline-all-stringops -rdynamic -I ./deps/udns-0.0.9/
LFLAGS=-ldl -lm -lpthread
CC=gcc -D_GNU_SOURCE
RM=rm -f

all: aped

aped: $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(EXEC) $(LFLAGS) ./deps/udns-0.0.9/libudns.a -I ./deps/udns-0.0.9/

install:
	install -d $(bindir) $(prefix)/modules/conf $(prefix)/modules/lib $(prefix)/scripts/commands $(prefix)/scripts/examples $(prefix)/scripts/framework $(prefix)/scripts/utils
	install -m 755 $(EXEC) $(bindir)
	install -m 644 bin/ape.default.conf $(bindir)
	install -m 644 AUTHORS COPYING README $(prefix)
	install -m 644 modules/conf/* $(prefix)/modules/conf/
	install -m 755 modules/lib/* $(prefix)/modules/lib/
	install -m 644 scripts/main.ape.js $(prefix)/scripts/
	install -m 644 scripts/commands/* $(prefix)/scripts/commands/
	install -m 644 scripts/examples/* $(prefix)/scripts/examples/
	install -m 644 scripts/framework/* $(prefix)/scripts/framework/
	install -m 644 scripts/utils/* $(prefix)/scripts/utils/

uninstall:
	$(RM) $(bindir)/aped

clean:
	$(RM) $(EXEC)

