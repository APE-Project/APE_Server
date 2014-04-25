#!/bin/bash

if [ -n "$1" ] && [ "$1" = "clean" ]; then
	make clean
	cd ./modules&&make clean
	cd ./deps/mysac&&make clean
	cd ../../../deps/udns-0.0.9&&make clean
	cd ../js/src&&make clean&&cd ../..
else
	rm -f ./src/configure.h ./modules/platform.mk ./modules/mysql.mk
	OS_TARGET=`uname -s`
	
	case "$OS_TARGET" in
		linux* | Linux*)
			HOST_OS=Linux
			echo "#define USE_EPOLL_HANDLER" > ./src/configure.h
			echo "LINUX_BUILD = 1" > ./modules/platform.mk;;
		Darwin*)
			HOST_OS=Darwin
			echo "#define USE_KQUEUE_HANDLER" > ./src/configure.h
			echo "DARWIN_BUILD = 1" > ./modules/platform.mk;;
		*)
			HOST_IS=Linux;;
	esac
	
	if [ -e "/usr/include/mysql/mysql.h" ]
	then
		echo "HAS_MYSQL = yes" > ./modules/mysql.mk
		echo "#define _USE_MYSQL 1" >> ./src/configure.h
	else
		echo "HAS_MYSQL = 0" > ./modules/mysql.mk
		echo "#undef _USE_MYSQL" >> ./src/configure.h
	fi
	#echo "STAGING_DEBUG=1" > build.mk
	echo "STAGING_RELEASE=1" > build.mk
	make
fi