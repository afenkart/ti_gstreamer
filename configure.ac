AC_PREREQ(2.59)
AC_INIT(libGStreamer, 0.1.0, rostislav.simonik@streamunlimited.com)
AC_CONFIG_SRCDIR([src/ti_da830.c])
AC_CONFIG_HEADER([autoconfig.h])
AC_CONFIG_AUX_DIR([configure_aux])
AM_INIT_AUTOMAKE([1.9 -Wall])
AC_CONFIG_MACRO_DIR([m4])


AM_PROG_LIBTOOL
AC_PROG_CC
AC_PROG_CPP
AC_PROG_INSTALL
AC_PROG_MAKE_SET
AM_PROG_CC_C_O

# Checks for libraries.

# Checks for header files.
AC_HEADER_STDC
AC_HEADER_SYS_WAIT
AC_CHECK_HEADERS([fcntl.h stdlib.h string.h sys/time.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_HEADER_TIME


# Checks for gstreamer flags and libs
PKGCONFIG_PROG_FOUND=0
if test -n "$PKGCONFIG_SEARCH_CONFIG"; then
	AC_MSG_NOTICE([You specified $PKGCONFIG_SEARCH_CONFIG for pkg-config])
	if test -f "$PKGCONFIG_SEARCH_CONFIG"; then
		pkgconfig="$PKGCONFIG_SEARCH_CONFIG"
		PKGCONFIG_PROG_FOUND=1
	else
		AC_MSG_ERROR(["$PKGCONFIG_SEARCH_CONFIG" not found])
	fi
else
	AC_PATH_PROG(pkgconfig, pkg-config, no)
	if test "x$pkgconfig" != xno; then
		PKGCONFIG_PROG_FOUND=1
	fi
fi

if test "$PKGCONFIG_PROG_FOUND" -eq 1; then
	PKGCONFIG_TEST=`${pkgconfig} --version 2>/dev/null`
	if test -z "$PKGCONFIG_TEST"; then
		AC_MSG_ERROR([${pkgconfig} could not be executed or returned invalid values])
	fi

	AC_MSG_CHECKING([gstreamer-0.10 cflags])
	GSTREAMER_CFLAGS=`${pkgconfig} --cflags gstreamer-0.10`
	AC_MSG_RESULT($GSTREAMER_CFLAGS)

	AC_MSG_CHECKING([gstreamer-0.10 libs])
	GSTREAMER_LIBS=`${pkgconfig} --libs gstreamer-0.10`
	AC_MSG_RESULT($GSTREAMER_LIBS)

	AC_SUBST(GSTREAMER_CFLAGS)
	AC_SUBST(GSTREAMER_LIBS)
else
	AC_MSG_ERROR([pkg-config not found, unable to configure gstreamer])
fi

# Checks for library functions.
AC_FUNC_FORK
AC_FUNC_SELECT_ARGTYPES
AC_CHECK_FUNCS([mkfifo select strerror])

AC_CONFIG_FILES([Makefile
                 build/Makefile])
AC_OUTPUT
