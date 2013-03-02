dnl -*- Autoconf -*-
dnl This file is part of mysql_slowlogd.
dnl (C) 2012 Groupon, Inc.
dnl
dnl Process this file with autoconf to produce a configure script.
dnl
AC_PREREQ(2.57)
AC_INIT([mysql_slowlogd], [1.0])
AM_INIT_AUTOMAKE([-Wall -Werror no-dependencies foreign])
AC_CONFIG_SRCDIR([src/mysql_slowlogd.c])
AC_CONFIG_HEADER([config.h])

dnl ==== Check for programs ====================================================
CFLAGS="${CFLAGS=}"
CFLAGS="$CFLAGS -Wall -Werror -g"
AC_PROG_CC

dnl ==== Configure vendor libraries ============================================
if test -d "$srcdir/vendor/libmicrohttpd"; then
  AC_CONFIG_SUBDIRS([vendor/libmicrohttpd])
fi

dnl ==== Check for headers =====================================================
AC_HEADER_STDC
AC_CHECK_HEADERS([sys/types.h sys/select.h sys/socket.h sys/stat.h stdlib.h fcntl.h unistd.h stdio.h string.h time.h signal.h getopt.h syslog.h],,AC_MSG_ERROR([Compiling mysql_slowlogd requires standard UNIX headers files]))

dnl ==== Check for typedefs, structures, and compiler characteristics ==========
AC_C_CONST
AC_TYPE_PID_T
AC_TYPE_SIZE_T

dnl ==== Check for library functions = =========================================
AC_FUNC_MALLOC

dnl ==== Output ===============================================================
dnl Output
AC_CONFIG_FILES([
  Makefile
  src/Makefile
])
AC_OUTPUT
