# Makefile for libkibadisk + kibaos-oobe-backend.
#
# This mirrors the exact compile/link sequence the KibaOS build script
# (build.sh) runs inline via gcc, extracted here so libkibadisk can be
# built and iterated on outside the full ISO build.
#
# libkibadisk deliberately has no external partitioning/mount-management
# library dependency -- see kiba_gpt.c, kiba_fs.c, kiba_udev.c for the
# from-scratch GPT writer (backed by sgdisk via argv/execvp, never a
# shell), mkfs/mount wrapper, and udev-settle replacement respectively.

CC       ?= gcc
CFLAGS   ?= -O2 -Wall
AR       ?= ar
PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin

LIB_SRCS := kiba_gpt.c kiba_fs.c kiba_udev.c kiba_install_extract.c kiba_install_finish.c
LIB_OBJS := $(LIB_SRCS:.c=.o)
LIB      := libkibadisk.a
BACKEND  := kibaos-oobe-backend

.PHONY: all clean install

all: $(BACKEND)

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $(LIB_OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BACKEND): kibaos_oobe_backend_main.c $(LIB)
	$(CC) $(CFLAGS) -o $@ kibaos_oobe_backend_main.c -L. -lkibadisk

install: $(BACKEND)
	install -D -m 755 $(BACKEND) $(DESTDIR)$(BINDIR)/$(BACKEND)

clean:
	rm -f $(LIB_OBJS) $(LIB) $(BACKEND)
