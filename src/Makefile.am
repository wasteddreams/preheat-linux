## Source Makefile for Preheat

bin_PROGRAMS = preheat

preheat_SOURCES = \
	daemon/main.c \
	daemon/daemon.c \
	daemon/daemon.h \
	daemon/signals.c \
	daemon/signals.h \
	daemon/pause.c \
	daemon/pause.h \
	daemon/session.c \
	daemon/session.h \
	daemon/stats.c \
	daemon/stats.h \
	config/config.c \
	config/config.h \
	config/confkeys.h \
	config/blacklist.c \
	config/blacklist.h \
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
	state/state_exe.c \
	state/state_exe.h \
	state/state_family.c \
	state/state_family.h \
	state/state_io.c \
	state/state_io.h \
	state/state_map.c \
	state/state_map.h \
	state/state_markov.c \
	state/state_markov.h \
	utils/logging.c \
	utils/logging.h \
	utils/crc32.c \
	utils/crc32.h \
	utils/pattern.c \
	utils/pattern.h \
	utils/desktop.c \
	utils/desktop.h \
	utils/seeding.c \
	utils/seeding.h \
	utils/lib_scanner.c \
	utils/lib_scanner.h

preheat_CPPFLAGS = \
	-I$(top_srcdir)/include \
	$(GLIB_CFLAGS) \
	-DSYSCONFDIR='"$(sysconfdir)"' \
	-DPKGLOCALSTATEDIR='"$(pkglocalstatedir)"' \
	-DLOGDIR='"$(logdir)"' \
	-DPACKAGE='"$(PACKAGE)"'

preheat_LDADD = $(GLIB_LIBS) -lm

# Compiler flags: warnings + maximum optimization
AM_CFLAGS = -Wall -Wextra -O3 -march=native -flto -funroll-loops -fno-strict-aliasing
