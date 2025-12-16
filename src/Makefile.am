## Source Makefile for Preheat

bin_PROGRAMS = preheat

preheat_SOURCES = \
	daemon/main.c \
	daemon/daemon.c \
	daemon/daemon.h \
	daemon/signals.c \
	daemon/signals.h \
	config/config.c \
	config/config.h \
	config/confkeys.h \
	monitor/proc.c \
	monitor/proc.h \
	monitor/spy.c \
	monitor/spy.h \
	predict/prophet.c \
	predict/prophet.h \
	readahead/readahead.c \
	readahead/readahead.h \
	state/state.c \
	state/state.h \
	utils/logging.c \
	utils/logging.h \
	utils/crc32.c \
	utils/crc32.h

preheat_CPPFLAGS = \
	-I$(top_srcdir)/include \
	$(GLIB_CFLAGS) \
	-DSYSCONFDIR='"$(sysconfdir)"' \
	-DPKGLOCALSTATEDIR='"$(pkglocalstatedir)"' \
	-DLOGDIR='"$(logdir)"' \
	-DPACKAGE='"$(PACKAGE)"'

preheat_LDADD = $(GLIB_LIBS) -lm

# Compiler flags: warnings + optimization
AM_CFLAGS = -Wall -Wextra -O2 -fno-strict-aliasing
