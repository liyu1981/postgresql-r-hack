#!/bin/bash

# --enable-replication is now default option. No need to do again.

# debug version
./configure --prefix=/home/yli/TestCamp/postrep-dev --exec-prefix=/home/yli/TestCamp/postrep-dev --enable-debug --enable-cassert
./spread/configure --prefix=/home/yli/TestCamp/postrep-dev --exec-prefix=/home/yli/TestCamp/postrep-dev --enable-debug --enable-cassert
#release version
# ./configure --prefix=/home/yli/TestCamp/postrep-dev --exec-prefix=/home/yli/TestCamp/postrep-dev
# ./spread/configure --prefix=/home/yli/TestCamp/postrep-dev --exec-prefix=/home/yli/TestCamp/postrep-dev