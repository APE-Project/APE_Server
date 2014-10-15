prefix		= /usr/share/APE_Server
bindir		= $(prefix)/bin
tmpdir		= src/build

OBJ=$(tmpdir)/base64.o $(tmpdir)/channel.o $(tmpdir)/cmd.o $(tmpdir)/config.o $(tmpdir)/dns.o $(tmpdir)/entry.o $(tmpdir)/event_epoll.o $(tmpdir)/event_kqueue.o $(tmpdir)/event_select.o $(tmpdir)/events.o $(tmpdir)/extend.o $(tmpdir)/handle_http.o $(tmpdir)/hash.o $(tmpdir)/http.o $(tmpdir)/json.o $(tmpdir)/json_parser.o $(tmpdir)/log.o $(tmpdir)/md5.o $(tmpdir)/parser.o $(tmpdir)/pipe.o $(tmpdir)/plugins.o  $(tmpdir)/raw.o $(tmpdir)/servers.o $(tmpdir)/sha1.o $(tmpdir)/sock.o $(tmpdir)/ticks.o $(tmpdir)/transports.o $(tmpdir)/users.o $(tmpdir)/utils.o
# $(tmpdir)/proxy.o
TARGET=aped
EXEC=bin/$(TARGET)
UDNS=./deps/udns-0.0.9/libudns.a
include ./build.mk
ifdef STAGING_DEBUG
DEBUGFLAGS=-g -ggdb
PROFILEFLAGS=-pg -profile
# -fdump-rtl-expand
endif
CFLAGS=-Wall -O2 -minline-all-stringops -I ./deps/udns-0.0.9/
LFLAGS=-rdynamic -ldl -lm -lpthread -lrt
CC=gcc -D_GNU_SOURCE
RM=rm -f

all: $(EXEC)

SRC=src/entry.c src/sock.c src/hash.c src/handle_http.c src/cmd.c src/users.c src/channel.c src/config.c src/json.c src/json_parser.c src/plugins.c src/http.c src/extend.c src/utils.c src/ticks.c src/base64.c src/pipe.c src/raw.c src/events.c src/event_kqueue.c src/event_epoll.c src/event_select.c src/transports.c src/servers.c src/dns.c src/sha1.c src/log.c src/parser.c src/md5.c

$(EXEC): $(OBJ) $(UDNS) modules
	@$(CC) $(OBJ) -o $(EXEC) $(LFLAGS) $(UDNS)
ifdef STAGING_RELEASE
	@strip $(EXEC)
endif
	@echo done $(EXEC)

$(tmpdir)/base64.o:			src/base64.c src/base64.h src/utils.h |$(tmpdir)
$(tmpdir)/channel.o:		src/channel.c src/channel.h src/main.h src/pipe.h src/users.h src/extend.h src/json.h src/hash.h src/utils.h src/raw.h src/plugins.h |$(tmpdir)
$(tmpdir)/cmd.o:			src/cmd.c src/cmd.h src/users.h src/handle_http.h src/sock.h src/main.h src/transports.h src/json.h src/config.h src/utils.h src/proxy.h src/raw.h |$(tmpdir)
$(tmpdir)/config.o:			src/config.c src/config.h src/utils.h |$(tmpdir)
$(tmpdir)/dns.o:			src/dns.c src/dns.h src/main.h src/sock.h src/events.h src/utils.h src/ticks.h |$(tmpdir)
$(tmpdir)/entry.o:			src/entry.c src/plugins.h src/main.h src/sock.h src/config.h src/cmd.h src/channel.h src/utils.h src/ticks.h src/proxy.h src/events.h src/transports.h src/servers.h src/dns.h src/log.h |$(tmpdir)
$(tmpdir)/event_epoll.o:	src/event_epoll.c src/events.h |$(tmpdir)
$(tmpdir)/event_kqueue.o:	src/event_kqueue.c src/events.h |$(tmpdir)
$(tmpdir)/event_select.o:	src/event_select.c src/events.h |$(tmpdir)
$(tmpdir)/events.o:			src/events.c src/events.h src/main.h src/utils.h src/configure.h |$(tmpdir)
$(tmpdir)/extend.o:			src/extend.c src/extend.h src/utils.h src/json.h |$(tmpdir)
$(tmpdir)/handle_http.o:	src/handle_http.c src/handle_http.h src/main.h src/users.h src/utils.h src/config.h src/cmd.h src/sock.h src/http.h src/parser.h src/md5.h src/sha1.h src/base64.h |$(tmpdir)
$(tmpdir)/hash.o:			src/hash.c src/hash.h src/users.h src/utils.h |$(tmpdir)
$(tmpdir)/http.o:			src/http.c src/http.h src/main.h src/sock.h src/utils.h src/dns.h src/log.h |$(tmpdir)
$(tmpdir)/json.o:			src/json.c src/json.h src/json_parser.h |$(tmpdir)
$(tmpdir)/json_parser.o:	src/json_parser.c src/json_parser.h |$(tmpdir)
$(tmpdir)/log.o:			src/log.c src/log.h src/main.h src/utils.h src/log.h src/config.h |$(tmpdir)
$(tmpdir)/md5.o:		 	src/md5.c src/md5.h |$(tmpdir)
$(tmpdir)/parser.o:			src/parser.c src/parser.h src/main.h src/http.h src/utils.h src/handle_http.h |$(tmpdir)
$(tmpdir)/pipe.o:			src/pipe.c src/pipe.h src/main.h src/users.h src/utils.h src/json.h |$(tmpdir)
$(tmpdir)/plugins.o:		src/plugins.c src/plugins.h src/main.h src/utils.h src/config.h modules/plugins.h |$(tmpdir)
$(tmpdir)/proxy.o:			src/proxy.c src/proxy.h src/main.h src/http.h src/sock.h src/pipe.h src/utils.h src/handle_http.h src/config.h src/base64.h src/pipe.h src/raw.h src/events.h src/log.h |$(tmpdir)
$(tmpdir)/raw.o:			src/raw.c src/raw.h src/main.h src/users.h src/channel.h src/proxy.h src/transports.h src/sock.h src/utils.h src/plugins.h src/pipe.h src/json.h src/json.c |$(tmpdir)
$(tmpdir)/servers.o:	 	src/servers.c src/servers.h src/main.h src/sock.h src/sock.c src/utils.h src/utils.c src/config.h src/utils.c src/http.h src/http.c src/handle_http.h src/handle_http.c src/transports.h src/transports.c src/parser.h src/main.h |$(tmpdir)
$(tmpdir)/sha1.o:			src/sha1.c src/sha1.h | $(tmpdir)
$(tmpdir)/sock.o:			src/sock.c src/sock.h src/main.h src/sock.h src/http.h src/users.h src/utils.h src/ticks.h src/proxy.h src/config.h src/raw.h src/events.h src/transports.h src/handle_http.h src/dns.h src/log.h src/parser.h |$(tmpdir)
$(tmpdir)/ticks.o:			src/ticks.c src/ticks.h src/main.h src/utils.h |$(tmpdir)
$(tmpdir)/transports.o:		src/transports.c src/transports.h src/main.h src/users.h src/config.h src/utils.h |$(tmpdir)
$(tmpdir)/users.o:			src/users.c src/users.h src/main.h src/channel.h src/json.h src/extend.h src/hash.h src/handle_http.h src/sock.h src/extend.h src/config.h src/json.h src/plugins.h src/pipe.h src/raw.h src/utils.h src/transports.h src/log.h |$(tmpdir)
$(tmpdir)/utils.o:			src/utils.c src/utils.h src/log.h |$(tmpdir)
#$(tmpdir)/main.o:		 	src/main.h src/hash.h |$(tmpdir)

$(tmpdir)/%.o:
	@echo compiling $@
	@$(CC) $(CFLAGS) $(DEBUGFLAGS) $(PROFILEFLAGS) -c $(<D)/$(<F) -o $@

$(UDNS):
	@cd ./deps/udns-0.0.9/&&make clean&&./configure&&make&&cd ../../

$(tmpdir):
	@mkdir -p $(tmpdir)

install:
	@install -d $(bindir) $(prefix)/modules/conf $(prefix)/modules/lib $(prefix)/scripts/commands $(prefix)/scripts/examples $(prefix)/scripts/framework $(prefix)/scripts/utils $(prefix)/scripts/test
	@install -m 755 $(EXEC) $(bindir)
	@install -m 644 bin/ape.conf $(bindir)
	@install -m 644 AUTHORS COPYING README $(prefix)
	@install -m 644 modules/conf/* $(prefix)/modules/conf/
	@install -m 755 modules/lib/* $(prefix)/modules/lib/
	@install -m 644 scripts/main.ape.js $(prefix)/scripts/
	@install -m 644 scripts/commands/* $(prefix)/scripts/commands/
	@install -m 644 scripts/examples/* $(prefix)/scripts/examples/
	@install -m 644 scripts/framework/* $(prefix)/scripts/framework/
	@install -m 644 scripts/utils/* $(prefix)/scripts/utils/
	@install -m 644 scripts/test/* $(prefix)/scripts/test/

modules: $(SRC)
	cd ./modules&&make&&cd ..

.PHONY: clean modules $(tmpdir)

uninstall:
	@$(RM) -R $(prefix)

clean:
	@$(RM) $(EXEC) $(tmpdir)/*.o
	@rmdir $(tmpdir)
	@cd ./modules&&make clean&&cd ..
