#!/bin/sh
SPREAD='spread'
PG_DATA=`readlink -f "./var/pgsql2"`

# first arg is to set debug mode
if [ $1 ]
then
    PRD_DEBUG=$1
else
    PRD_DEBUG=0
fi

# echo "PRD_DEBUG=$PRD_DEBUG"

if ps ax | grep -v grep | grep $SPREAD > /dev/null
then
    echo "Find spread running, fine :)"
else
    echo "Spread is not running, try to start it..."
    ./sbin/spread &
    echo "OK."
fi

if [ $PRD_DEBUG = 0 ]
then
    ./bin/postgres -D $PG_DATA &
else
    ./bin/postgres -D $PG_DATA -d $PRD_DEBUG &
fi

