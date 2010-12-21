#!/bin/sh

./bin/pg_ctl -D `readlink -f "./var/pgsql"` stop
