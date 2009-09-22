#!/bin/bash

cd ./libs/udns-0.0.9/
make clean && ./configure && make
cd ../nspr-4.8/mozilla/nsprpub/
make clean && ./configure && make
cd ../../../js/src/
make JS_DIST=../../nspr-4.8/mozilla/nsprpub/dist/ JS_THREADSAFE=1 -f Makefile.ref
cd ../../../
make
cd modules
make
