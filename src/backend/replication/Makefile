#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for src/backend/replication
#
# IDENTIFICATION
#    $PostgreSQL$
#
#-------------------------------------------------------------------------

subdir = src/backend/replication
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = walsender.o walreceiverfuncs.o walreceiver.o

ifeq ($(enable_replication), yes)
OBJS += gc_utils.o gc_egcs.o \
       cset.o local.o remote.o coordinator.o utils.o recovery.o
# defunct: gc_ensemble.o gc_spread.o
endif

include $(top_srcdir)/src/backend/common.mk
