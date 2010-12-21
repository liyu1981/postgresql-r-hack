#!/bin/sh

PG_DATA=./var/pgsql2

if [ ! -d $PG_DATA ];
then
	mkdir -p $PG_DATA
fi

PG_DATA=`readlink -f "./var/pgsql2"`

./bin/initdb -D $PG_DATA

cp $PG_DATA/postgresql.conf $PG_DATA/postgresql.conf.old
sed "s/#replication = on/replication = on/" $PG_DATA/postgresql.conf.old >$PG_DATA/postgresql.conf
rm $PG_DATA/postgresql.conf.old

