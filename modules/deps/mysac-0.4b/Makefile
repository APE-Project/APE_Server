
# get build version from the git tree in the form "lasttag-changes", and use "dev" if unknown
BUILDVER := $(shell ref=`(git describe --tags) 2>/dev/null` && ref=$${ref%-g*} && echo "$${ref\#v}")

CFLAGS = -DBUILDVER=$(BUILDVER) -I/usr/include/mysql -O0 -g -Wall
LDFLAGS = -g -lmysqlclient_r

OBJS = mysac.o mysac_net.o mysac_decode_field.o mysac_decode_row.o mysac_errors.o

build: make.deps
	$(MAKE) lib

pack:
	rm -rf /tmp/mysac-$(BUILDVER) >/dev/null 2>&1; \
	git clone . /tmp/mysac-$(BUILDVER) && \
	tar --exclude .git -C /tmp/ -vzcf mysac-$(BUILDVER).tar.gz mysac-$(BUILDVER) && \
	rm -rf /tmp/mysac-$(BUILDVER) >/dev/null 2>&1; \

lib: libmysac.a
#libmysac.so

libmysac.so: libmysac.a
	$(LD) -o libmysac.so -shared -soname libmysac.so.0.0 libmysac.a

libmysac.a: $(OBJS)
	$(AR) -rcv libmysac.a $(OBJS)

make.deps: *.c *.h
	for src in *.c; do \
		DEPS="$$(sed -e 's/^#include[ 	]"\(.*\)"/\1/; t; d;' $$src | xargs echo)"; \
		echo "$${src//.c/.o}: $$src $$DEPS"; \
	done > make.deps

#exemple: libmysac.a
#	$(MAKE) -C exemple CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)"

clean:
	rm -rf make.deps libmysac.so libmysac.a main.o man html $(OBJS)

doc:
	doxygen mysac.doxygen

api:
	echo " " > header_file
	rm -rf apidoc >/dev/null 2>&1
	doxygen mysac-api.doxygen

include make.deps

#.PHONY: exemple
