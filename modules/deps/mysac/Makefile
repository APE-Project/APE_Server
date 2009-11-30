#
# Copyright (c) 2009 Thierry FOURNIER
#
# This file is part of MySAC.
#
# MySAC is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License
#
# MySAC is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with MySAC.  If not, see <http://www.gnu.org/licenses/>.
#

# Mysql lib directory
# exemple: <path>/mysql-5.1.41/libmysql_r/.libs
MYSQL_LIB := /usr/lib

# Mysql include directory
# exemple: <path>/mysql-5.1.41/include
MYSQL_INC := /usr/include/mysql

# get build version from the git tree in the form "lasttag-changes", 
# and use "VERSION" file if unknown.
BUILDVER := $(shell ./mysac_ver)

CFLAGS = -DBUILDVER=$(BUILDVER) -I$(MYSQL_INC) -O0 -g -Wall -fPIC
LDFLAGS = -g -L$(MYSQL_LIB) -lmysqlclient_r

OBJS = mysac.o mysac_net.o mysac_decode_field.o mysac_decode_row.o mysac_encode_values.o mysac_errors.o

build: make.deps
	$(MAKE) lib

pack:
	rm -rf /tmp/mysac-$(BUILDVER) >/dev/null 2>&1; \
	git clone . /tmp/mysac-$(BUILDVER) && \
	echo "$(BUILDVER)" > VERSION; \
	cp VERSION /tmp/mysac-$(BUILDVER); \
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

exemple: libmysac.a
	$(MAKE) -C exemple CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)"

clean:
	rm -rf make.deps libmysac.so libmysac.a main.o man html $(OBJS)

doc:
	doxygen mysac.doxygen

api:
	echo " " > header_file
	rm -rf apidoc >/dev/null 2>&1
	doxygen mysac-api.doxygen

include make.deps
