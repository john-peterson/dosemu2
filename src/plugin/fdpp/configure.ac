dnl Process this file with autoconf to produce a configure script.
AC_INIT
AC_CONFIG_HEADERS([_fdppconf.h])
AC_CONFIG_FILES([Makefile.conf])
PKG_PROG_PKG_CONFIG
AC_ARG_WITH([fdpp-build-path], AS_HELP_STRING([--with-fdpp-build-path=DIR],
        [use alternate fdpp dir]),
  [
    fdpp_lib_dir=$withval
    AC_ARG_WITH([fdpp-include-path],
        AS_HELP_STRING([--with-fdpp-include-path=DIR],
            [set fdpp include dir]), fdpp_inc_dir="$withval",
        AC_MSG_ERROR([use --with-fdpp-include-path]))
    export PKG_CONFIG_PATH="$fdpp_lib_dir"
    export PKG_CONFIG="$PKG_CONFIG \
        --define-variable=includedir=$fdpp_inc_dir \
        --define-variable=fdpplibdir=$fdpp_lib_dir \
        --define-variable=fdpprlibdir=$fdpp_lib_dir"
    AC_DEFINE_UNQUOTED(FDPP_KERNEL_DIR, "$fdpp_lib_dir")
  ],
  [
    if test "x$prefix" = xNONE; then
      if test "x$PKG_CONFIG_PATH" = x
      then
        export PKG_CONFIG_PATH="$ac_default_prefix/lib/pkgconfig"
      else
        export PKG_CONFIG_PATH="$PKG_CONFIG_PATH:$ac_default_prefix/lib/pkgconfig"
      fi
    fi
  ]
)
PKG_CHECK_MODULES([FDPP], [fdpp >= 1.6],,
    AC_MSG_ERROR([fdpp installation not found or too old]))
PKG_CHECK_MODULES([VALGRIND], [valgrind], AC_DEFINE(HAVE_VALGRIND),
  AC_MSG_NOTICE([valgrind support disabled]))
AH_TEMPLATE([HAVE_VALGRIND],
[Define this if you want valgrind integration])
AH_TEMPLATE([FDPP_KERNEL_DIR],
[Define this to your custom fdpp location])
AC_OUTPUT

sed -E 's/#define PACKAGE_.+//' _fdppconf.h >fdppconf.hh
