# notmuchfs - A virtual maildir file system for notmuch queries
#
# Copyright © 2012 Tim Stoakes
#
# This file is part of notmuchfs.
#
# Notmuchfs is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with notmuchfs.  If not, see http://www.gnu.org/licenses/ .
#
# Authors: Tim Stoakes <tim@stoakes.net>

PKG_CONFIG ?= pkg-config

CFLAGS = -g -O2 -std=c99 -Wall -Wextra -Werror -D_FILE_OFFSET_BITS=64 $(shell $(PKG_CONFIG) --cflags fuse3)

OBJS = notmuchfs.o

LIBS = -lnotmuch $(shell $(PKG_CONFIG) --libs fuse3)

all: notmuchfs

notmuchfs: $(OBJS)
	$(CC) -o $@ $+ $(LIBS)

clean:
	rm -f *.o *.dep notmuchfs

%.o : %.c
	$(COMPILE.c) -MD -o $@ $<
	@cp $*.d $*.dep; \
    sed -e 's/#.*//' -e 's/^[^:]*: *//' -e 's/ *\\$$//' \
        -e '/^$$/ d' -e 's/$$/ :/' < $*.d >> $*.dep; \
    rm -f $*.d

-include $(OBJS:.o=.dep)
