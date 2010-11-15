#
# Makefile for storage/ipc
#
# $PostgreSQL$
#

subdir = src/backend/storage/ipc
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

# seems to be required 1999/07/22 bjm
ifeq "$(findstring alpha,$(host_cpu))" "alpha"
ifeq "$(GCC)" "yes"
override CFLAGS+= -fno-inline
endif
endif

OBJS = imsg.o ipc.o ipci.o istream.o pmsignal.o procarray.o procsignal.o \
	shmem.o shmqueue.o sinval.o sinvaladt.o standby.o wamalloc.o

ifeq ($(enable_replication), yes)
OBJS+= buffer.o
endif

include $(top_srcdir)/src/backend/common.mk
