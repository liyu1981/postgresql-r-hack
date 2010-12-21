#!/bin/bash

# --enable-replication is now default option. No need to do again.

usage()
{
	echo "Usage: $0 -d <install-prefix> -r -p -s"
	echo "  -d <install-prefix>: required : should be absolute path"
	echo "  -r                 : option   : turn on release build"
	echo "  -p                 : option   : turn off configure postgres-r"
	echo "  -s                 : option   : turn off configure spread"
	exit 1
}

config-postgres()
{
	if [ ! -f "configure" ];
	then
		echo "Can not find Postgres-R configure, try autoconf ..."
		autoconf
		echo "OK."
	fi

	if [ $release_flag -eq 0 ];
	then
		./configure --prefix=$install_prefix --exec-prefix=$install_prefix --enable-debug --enable-cassert --enable-coordinatordebug
	else
		./configure --prefix=$install_prefix --exec-prefix=$install_prefix
	fi
}

config-spread()
{
	cd spread

	if [ ! -f "configure" ];
	then
		echo "Can not find Spread configure, try autoconf ..."
		autoconf
		echo "OK."
	fi

	if [ $release_flag -eq 0 ];
	then
		./configure --prefix=$install_prefix --exec-prefix=$install_prefix --enable-debug --enable-cassert
	else
		./configure --prefix=$install_prefix --exec-prefix=$install_prefix
	fi

	cd ..
}

###########################################################################################
# main starts here

release_flag=0
install_prefix=./postrep-dev
postgresr_flag=1
spread_flag=1

if [ $# -lt 1 ]; 
then
	usage
fi

while getopts d:r opt
do
	case "$opt" in
		d) install_prefix="$OPTARG";;
		r) release_flag=1;;
		p) postgresr_flag=0;;
		s) spread_flag=0;;
		\?) usage;;
	esac
done

install_prefix=`readlink -f "$install_prefix"`

#echo $release_flag
#echo $install_prefix
#echo $postgresr_flag
#echo $spread_flag

#exit

if [ $postgresr_flag -eq 1 ];
then
	echo "start configure postgres-r"
	config-postgres
	echo "done."
fi

if [ $spread_flag -eq 1 ];
then
	echo "start configure spread"
	config-spread
	echo "done."
fi

echo "Bye. :)"
