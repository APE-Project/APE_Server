#!/bin/bash

cd ./libs/udns-0.0.9/
make clean && ./configure && make
cd ../js1.8/src/
autoconf && ./configure && make && make install
ldconfig
cd ../../../
make
cd modules
make
