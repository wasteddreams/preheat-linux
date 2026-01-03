##                                               -*- Autoconf -*-
## Process this file with autoconf to produce a configure script.
##############################################################################

AC_PREREQ(2.69)
AC_INIT([preheat], [1.0.1], [https://github.com/wasteddreams/preheat-linux/issues])
AC_CONFIG_SRCDIR(src/daemon/main.c)
AC_CONFIG_HEADERS(config.h)
AM_INIT_AUTOMAKE([foreign subdir-objects -Wall -Wno-portability])
AC_GNU_SOURCE

PACKAGE_SUMMARY="Adaptive readahead daemon for Debian-based distributions"
AC_SUBST(PACKAGE_SUMMARY)

## Build options

# Preheat extensions (disabled by default for compatibility)
AC_ARG_ENABLE([preheat-extensions],
    AS_HELP_STRING([--enable-preheat-extensions],
        [Enable Preheat scoring and prediction extensions (default: no)]),
    [enable_preheat_extensions=$enableval],
    [enable_preheat_extensions=no])

if test "x$enable_preheat_extensions" = "xyes"; then
    AC_DEFINE([ENABLE_PREHEAT_EXTENSIONS], [1],
        [Define to 1 to enable Preheat extensions])
fi
AM_CONDITIONAL([ENABLE_PREHEAT_EXTENSIONS], [test "x$enable_preheat_extensions" = "xyes"])

# Debug mode
AC_ARG_ENABLE([debug],
    AS_HELP_STRING([--enable-debug],
        [Enable debug mode with verbose logging (default: no)]),
    [enable_debug=$enableval],
    [enable_debug=no])

if test "x$enable_debug" = "xyes"; then
    AC_DEFINE([DEBUG], [1], [Define to 1 to enable debug mode])
    CFLAGS="$CFLAGS -g -O0"
else
    CFLAGS="$CFLAGS -O2"
fi

# Checks for programs
AC_PROG_CC
AC_PROG_CC_C99
AC_PROG_INSTALL
AC_PROG_SED
AC_PROG_MAKE_SET

# Checks for header files
AC_HEADER_STDC
AC_HEADER_TIME
AC_CHECK_HEADERS([stdlib.h string.h strings.h inttypes.h sys/times.h unistd.h])

# Checks for typedefs and structures
AC_C_CONST
AC_C_STRINGIZE
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_OFF_T

# Compiler characteristics
if test "x$GCC" = "xyes"; then
  CFLAGS="$CFLAGS -Wall -Wextra -std=c99 -pedantic"
fi

# Checks for library functions
AC_CHECK_FUNCS(readahead,,[
    AC_MSG_ERROR([readahead(2) system call required but not found.
*** get yourself a recent Linux and glibc
*** Exiting])
])

AC_TYPE_SIGNAL
AC_CHECK_HEADERS([linux/fs.h])
AC_CHECK_FUNCS([fdatasync fsync memset mkdir strchr strdup strerror])

# Check for required libraries
PKG_CHECK_MODULES(GLIB, glib-2.0 >= 2.16)
AC_SUBST(GLIB_CFLAGS)
AC_SUBST(GLIB_LIBS)

# Directories
pkgdocdir='${datadir}/doc/'${PACKAGE_NAME}
initddir='${sysconfdir}/init.d'
systemddir='${prefix}/lib/systemd/system'
logrotatedir='${sysconfdir}/logrotate.d'
logdir='${localstatedir}/log'
pkglocalstatedir='${localstatedir}/lib/'${PACKAGE_NAME}
pkgconfdir='${sysconfdir}/'${PACKAGE_NAME}'.d'

AC_SUBST(pkgdocdir)
AC_SUBST(initddir)
AC_SUBST(systemddir)
AC_SUBST(logrotatedir)
AC_SUBST(logdir)
AC_SUBST(pkglocalstatedir)
AC_SUBST(pkgconfdir)

# Generate output
# Systemd unit directory
AC_ARG_WITH([systemdsystemunitdir],
    [AS_HELP_STRING([--with-systemdsystemunitdir=DIR], [Directory for systemd service files])],
    [],
    [with_systemdsystemunitdir=\${prefix}/lib/systemd/system])
AC_SUBST([systemdsystemunitdir], [$with_systemdsystemunitdir])

# Logrotate directory
AC_ARG_WITH([logrotate],
    [AS_HELP_STRING([--with-logrotate=DIR], [Directory for logrotate configuration])],
    [],
    [with_logrotate=\${sysconfdir}/logrotate.d])
AC_SUBST([logrotate], [$with_logrotate])

AC_CONFIG_FILES([
    Makefile
    src/Makefile
    tools/Makefile
])

AC_OUTPUT

echo "
Preheat $VERSION configuration:
====================================
  Prefix:              ${prefix}
  C compiler:          ${CC}
  CFLAGS:              ${CFLAGS}
  
  Preheat extensions:  ${enable_preheat_extensions}
  Debug mode:          ${enable_debug}
  
  State directory:     ${pkglocalstatedir}
  Config directory:    ${sysconfdir}
  Log directory:       ${logdir}
"
