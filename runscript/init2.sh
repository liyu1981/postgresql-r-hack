#!/bin/sh
PG_DATA=`readlink -f "./var/pgsql2"`

if [ ! -d $PG_DATA ];
then
	mkdir -p $PG_DATA
fi

./bin/initdb -D $PG_DATA
