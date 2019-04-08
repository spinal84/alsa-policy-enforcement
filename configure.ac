AC_INIT([alsa-policy-enforcement], [0.27], [spinal.by@gmail.com])
AM_INIT_AUTOMAKE([foreign subdir-objects dist-xz])

AC_PROG_CC
AC_SEARCH_LIBS([floor], [m])
PKG_CHECK_MODULES([DEPS], [glib-2.0 dbus-glib-1 alsa])

AC_OUTPUT([Makefile])
