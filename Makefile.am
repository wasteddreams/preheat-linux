##                                               -*- Automake -*-
## Process this file with automake to produce Makefile.in
##############################################################################

SUBDIRS = src tools tests

EXTRA_DIST = \
	README.md \
	LICENSE \
	CHANGELOG.md \
	config/preheat.conf.default \
	config/apps.list.example \
	config/blacklist.example \
	scripts/install.sh \
	debian/preheat.service.in \
	debian/preheat.logrotate

# Systemd service file
systemdsystemunit_DATA = debian/preheat.service

# Logrotate configuration  
logrotate_DATA = debian/preheat.logrotate

# Substitute paths in service file
debian/preheat.service: debian/preheat.service.in
	$(AM_V_GEN)$(SED) \
		-e 's|@bindir[@]|$(bindir)|g' \
		-e 's|@localstatedir[@]|$(localstatedir)|g' \
		< $< > $@

CLEANFILES = debian/preheat.service

# Build artifacts
MAINTAINERCLEANFILES = \
	$(srcdir)/INSTALL \
	$(srcdir)/aclocal.m4 \
	$(srcdir)/compile \
	$(srcdir)/config.guess \
	$(srcdir)/config.h.in \
	$(srcdir)/config.sub \
	$(srcdir)/configure \
	$(srcdir)/depcomp \
	$(srcdir)/install-sh \
	$(srcdir)/missing \
	`find "$(srcdir)" -type f -name Makefile.in -print` \
	`find "$(srcdir)" -type f -name "*~" -print`

# Create directories on install
install-data-hook:
	$(MKDIR_P) $(DESTDIR)$(localstatedir)/lib/preheat
	$(MKDIR_P) $(DESTDIR)$(localstatedir)/log
	$(MKDIR_P) $(DESTDIR)$(sysconfdir)
	@if [ ! -f $(DESTDIR)$(sysconfdir)/preheat.conf ]; then \
		$(INSTALL_DATA) $(srcdir)/config/preheat.conf.default \
			$(DESTDIR)$(sysconfdir)/preheat.conf; \
	fi
	@if [ -z "$(DESTDIR)" ] && command -v systemctl >/dev/null 2>&1; then \
		echo "Reloading systemd daemon..."; \
		systemctl daemon-reload || true; \
		echo "Enabling preheat service..."; \
		systemctl enable preheat.service || true; \
		echo "Starting preheat service..."; \
		systemctl start preheat.service || true; \
	fi

# Syntax checking
.PHONY: check-syntax
check-syntax:
	$(CC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) \
		$(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CFLAGS) $(CFLAGS) \
		-fsyntax-only $(CHK_SOURCES)
