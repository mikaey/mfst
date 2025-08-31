AC_INIT([mfst],[0.4])

AC_ARG_ENABLE([ncurses], AS_HELP_STRING([--disable-ncurses], [Disable the ncurses frontend]))

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_SRCDIR([mfst.c])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AC_LANG([C])
AC_PROG_CC
AC_CONFIG_FILES([Makefile])
AC_SUBST([PTHREADS],[])
AC_CHECK_HEADER([pthread.h],
[
    AC_CHECK_LIB([pthread], [pthread_create],
    [
        AC_DEFINE([HAVE_PTHREADS], [], [Defined if the target system supports multithreading.])
    ] [
        AC_SUBST([PTHREADS],[-pthread])
    ],
    [
        AC_MSG_ERROR([Target system does not appear to support multithreading.])
    ])
],
[
    AC_MSG_ERROR([Could not find pthreads.h])
])

AS_IF([test "x$enable_ncurses" != "xno"],
[
    PKG_CHECK_MODULES([ncurses], [ncurses],
    [
        AC_DEFINE([HAVE_NCURSES], [], [Defined if ncurses is available.])
    ])
])

PKG_CHECK_MODULES([libudev], [libudev], [AC_DEFINE([HAVE_UDEV], [], [Defined if udev is available])])
PKG_CHECK_MODULES([jsonc], [json-c], [AC_DEFINE([HAVE_JSONC], [], [Defined if json-c is available.])])
PKG_CHECK_MODULES([MariaDB], [mariadb], [AC_DEFINE([HAVE_MARIADB], [], [Defined if MariaDB client libraries are available.])])
PKG_CHECK_MODULES([uuid], [uuid], [AC_DEFINE([HAVE_UUID], [], [Define if uuid exists.])])

AC_SUBST([ncurses_CFLAGS])
AC_SUBST([ncurses_LIBS])
AC_SUBST([libudev_CFLAGS])
AC_SUBST([libudev_LIBS])
AC_SUBST([JSONC_CFLAGS])
AC_SUBST([JSONC_LIBS])
AC_SUBST([MariaDB_CFLAGS])
AC_SUBST([MariaDB_LIBS])
AC_SUBST([uuid_CFLAGS])
AC_SUBST([uuid_LIBS])
AC_OUTPUT
