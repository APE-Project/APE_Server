#!/bin/bash

OS_TARGET=`uname -s`

case "$OS_TARGET" in
        linux* | Linux*)
        HOST_OS=Linux
		echo "#define USE_EPOLL_HANDLER" > ./src/configure.h
		echo "LINUX_BUILD = 1" > ./modules/plateform.mk;;
        Darwin*)
        HOST_OS=Darwin
		echo "#define USE_KQUEUE_HANDLER" > ./src/configure.h
		echo "DARWIN_BUILD = 1" > ./modules/plateform.mk;;
        *)
        HOST_IS=Linux;;
esac


cd ./deps/udns-0.0.9/
make clean && ./configure && make
cd ../js/src/
./configure && make
cd ../../../
make
cd modules/deps/mysac/
make
cd ../../
make
