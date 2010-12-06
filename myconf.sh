#!/bin/bash

# --enable-replication is now default option. No need to do again.

if [ ! -f "configure" ];
then
	echo "Can not find Postgres-R configure, try autoconf ..."
	autoconf
	echo "OK."
fi

# debug version
./configure --prefix=/home/yli/TestCamp/postrep-dev --exec-prefix=/home/yli/TestCamp/postrep-dev --enable-debug --enable-cassert
#release version
# ./configure --prefix=/home/yli/TestCamp/postrep-dev --exec-prefix=/home/yli/TestCamp/postrep-dev

cd spread

if [ ! -f "configure" ];
then
	echo "Can not find Spread configure, try autoconf ..."
	autoconf
	echo "OK."
fi

# debug version
./configure --prefix=/home/yli/TestCamp/postrep-dev --exec-prefix=/home/yli/TestCamp/postrep-dev --enable-debug --enable-cassert
#release version
# ./configure --prefix=/home/yli/TestCamp/postrep-dev --exec-prefix=/home/yli/TestCamp/postrep-dev

cd ..
