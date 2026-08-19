# Copyright (C) 2026 Shanghai Symthosm Intelligent Technology Co., Ltd.
#                   上海先道智觉科技有限责任公司
# Author: Lu Haoran <luhaoran1981@icloud.com>
#         芦浩然
# SPDX-License-Identifier: GPL-2.0-or-later

VERSION       = 0.2.0
SOVER_MAJOR   = 0

CC            = gcc
AR            = ar
CFLAGS        = -Wall -Wextra -std=c11 -pthread -O2 -g -fPIC \
                -DTASKHEALTH_VERSION=\"$(VERSION)\" \
                -I $(SRCDIR) -I $(DAEMONDIR) -I $(INCDIR)
LDFLAGS       = -pthread

prefix         = /usr/local
bindir         = $(prefix)/bin
sbindir        = $(prefix)/sbin
libdir         = $(prefix)/lib
includedir     = $(prefix)/include
pkgconfigdir   = $(libdir)/pkgconfig
mandir         = $(prefix)/share/man
systemdunitdir = $(prefix)/lib/systemd/system

SRCDIR        = src
INCDIR        = include
TESTDIR       = test
DEMODIR       = demo
DAEMONDIR     = daemon

LIBST         = libtaskhealth.a
LIBSO         = libtaskhealth.so
LIBSONAME     = $(LIBSO).$(SOVER_MAJOR)
LIBSOREAL     = $(LIBSO).$(VERSION)

LIB_OBJS      = $(SRCDIR)/taskhealth.o $(SRCDIR)/taskhealth_mutex.o
PUBLIC_HEADERS = $(SRCDIR)/taskhealth.h $(SRCDIR)/taskhealth_mutex.h
PROTOCOL_HEADER = $(INCDIR)/taskhealth/protocol.h
HEADERS       = $(PUBLIC_HEADERS) $(PROTOCOL_HEADER)
DAEMON_SRCS   = $(DAEMONDIR)/main.c $(DAEMONDIR)/server.c $(DAEMONDIR)/registry.c \
                $(DAEMONDIR)/watchdog.c $(DAEMONDIR)/probe.c $(DAEMONDIR)/alert.c
DAEMON_OBJS   = $(DAEMON_SRCS:.c=.o)
TARNAME       = taskhealth-$(VERSION).tar.gz

# default: static + shared library + daemon
all: $(LIBST) $(LIBSONAME) taskhealthd

$(LIBST): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(LIBSONAME): $(LIB_OBJS)
	$(CC) -shared -Wl,-soname,$(LIBSONAME) -o $(LIBSOREAL) $^ $(LDFLAGS)
	ln -sf $(LIBSOREAL) $(LIBSONAME)
	ln -sf $(LIBSONAME) $(LIBSO)

taskhealthd: $(DAEMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(SRCDIR)/taskhealth.o: $(SRCDIR)/taskhealth.c $(SRCDIR)/taskhealth.h $(PROTOCOL_HEADER)
	$(CC) $(CFLAGS) -c -o $@ $<

$(SRCDIR)/taskhealth_mutex.o: $(SRCDIR)/taskhealth_mutex.c $(SRCDIR)/taskhealth_mutex.h $(SRCDIR)/taskhealth_internal.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(DAEMONDIR)/%.o: $(DAEMONDIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# demo program
demo: $(DEMODIR)/demo.c $(LIBST) $(HEADERS)
	$(CC) $(CFLAGS) -o $(DEMODIR)/$@ $< -L. -ltaskhealth $(LDFLAGS)

# unit test (client library API only; needs a running taskhealthd
# on the default socket for the register round-trip tests)
test: $(TESTDIR)/test.c $(LIBST) $(HEADERS)
	$(CC) $(CFLAGS) -o $(TESTDIR)/$@ $< -L. -ltaskhealth $(LDFLAGS)

check: test
	LD_LIBRARY_PATH=.:$$LD_LIBRARY_PATH $(TESTDIR)/test

# code coverage
COV_DIR   = coverage
COV_CFLAGS = -Wall -Wextra -std=c11 -pthread -O0 -g -fPIC \
             -DTASKHEALTH_VERSION=\"$(VERSION)\" -I $(SRCDIR) --coverage

cov: clean
	$(CC) $(COV_CFLAGS) -c -o $(SRCDIR)/taskhealth.o $(SRCDIR)/taskhealth.c
	$(CC) $(COV_CFLAGS) -c -o $(SRCDIR)/taskhealth_mutex.o $(SRCDIR)/taskhealth_mutex.c
	$(AR) rcs $(LIBST) $(LIB_OBJS)
	$(CC) $(COV_CFLAGS) --coverage -o $(TESTDIR)/test $(TESTDIR)/test.c \
		-L. -ltaskhealth $(LDFLAGS)
	LD_LIBRARY_PATH=.:$$LD_LIBRARY_PATH $(TESTDIR)/test 2>/dev/null
	gcov -r $(SRCDIR)/taskhealth.c $(SRCDIR)/taskhealth_mutex.c
	@echo "  => taskhealth.c.gcov  taskhealth_mutex.c.gcov"

all-full: $(LIBST) $(LIBSONAME) taskhealthd demo test taskhealth.pc

clean:
	rm -f $(LIB_OBJS) $(DAEMON_OBJS) $(LIBST) $(LIBSOREAL) $(LIBSONAME) $(LIBSO) \
		taskhealthd \
		$(DEMODIR)/demo $(TESTDIR)/test \
		*.gcno *.gcda *.gcov $(SRCDIR)/*.gcno $(SRCDIR)/*.gcda \
		$(TESTDIR)/*.gcno $(TESTDIR)/*.gcda \
		$(DAEMONDIR)/*.gcno $(DAEMONDIR)/*.gcda

install: $(LIBST) $(LIBSONAME) taskhealthd demo
	install -d $(DESTDIR)$(libdir) $(DESTDIR)$(includedir) \
		$(DESTDIR)$(includedir)/taskhealth $(DESTDIR)$(bindir) \
		$(DESTDIR)$(pkgconfigdir) $(DESTDIR)$(sbindir) \
		$(DESTDIR)$(mandir)/man1 $(DESTDIR)$(mandir)/man8 \
		$(DESTDIR)$(mandir)/man7 $(DESTDIR)$(systemdunitdir)
	install -m 644 $(LIBST) $(DESTDIR)$(libdir)
	install -m 755 $(LIBSOREAL) $(DESTDIR)$(libdir)
	ln -sf $(LIBSOREAL) $(DESTDIR)$(libdir)/$(LIBSONAME)
	ln -sf $(LIBSONAME) $(DESTDIR)$(libdir)/$(LIBSO)
	install -m 644 $(PUBLIC_HEADERS) $(DESTDIR)$(includedir)
	install -m 644 $(PROTOCOL_HEADER) $(DESTDIR)$(includedir)/taskhealth
	install -m 644 taskhealth.pc $(DESTDIR)$(pkgconfigdir)
	install -m 755 taskhealthd $(DESTDIR)$(sbindir)
	install -m 755 $(DEMODIR)/demo $(DESTDIR)$(bindir)/taskhealth-demo
	install -m 644 man/taskhealthd.8 $(DESTDIR)$(mandir)/man8/taskhealthd.8
	install -m 644 man/taskhealth-demo.1 $(DESTDIR)$(mandir)/man1/taskhealth-demo.1
	install -m 644 man/taskhealth.7 $(DESTDIR)$(mandir)/man7/taskhealth.7
	install -m 644 contrib/systemd/taskhealthd.service \
		$(DESTDIR)$(systemdunitdir)/taskhealthd.service

# pkg-config file (generated at build time)
taskhealth.pc: taskhealth.pc.in
	sed -e 's|@prefix@|$(prefix)|g' \
	    -e 's|@libdir@|$(libdir)|g' \
	    -e 's|@includedir@|$(includedir)|g' \
	    -e 's|@VERSION@|$(VERSION)|g' \
	    $< > $@

# source tarball (for rpm build)
dist: clean
	mkdir -p taskhealth-$(VERSION)
	cp -r $(SRCDIR) $(INCDIR) $(TESTDIR) $(DEMODIR) $(DAEMONDIR) debian contrib man Makefile \
		taskhealth.pc.in LICENSE LICENSE.MIT README.md \
		taskhealth-$(VERSION)/
	tar czf $(TARNAME) taskhealth-$(VERSION)
	rm -rf taskhealth-$(VERSION)

# Debian package
deb:
	debuild -b -uc -us

# RPM package
rpm: dist
	rpmbuild -ta $(TARNAME)

.PHONY: all all-full clean check cov install demo test dist deb rpm
