dnl ---------------------------------------------------------------------------
dnl Macro: AC_CHECK_FIREBIRD
dnl Check for Firebird client library (libfbclient) and headers.
dnl Supports --with-firebird=PATH to specify a custom installation prefix,
dnl or --with-firebird-includes and --with-firebird-libs for fine-grained control.
dnl If no paths are given, falls back to fb_config if available.
dnl ---------------------------------------------------------------------------

AC_DEFUN([AC_CHECK_FIREBIRD],[

# Check for custom Firebird root directory
if test [ "x$with_firebird" != xyes -a "x$with_firebird" != xno ]
then
    ac_cv_firebird_root=`echo "$with_firebird" | sed -e 's+/$++'`
    if test [ -d "$ac_cv_firebird_root/include" -a \
              -d "$ac_cv_firebird_root/lib" ]
    then
        ac_cv_firebird_includes="$ac_cv_firebird_root/include"
        ac_cv_firebird_libs="$ac_cv_firebird_root/lib"
    elif test [ -x "$ac_cv_firebird_root/bin/fb_config" ]
    then
        fbconfig="$ac_cv_firebird_root/bin/fb_config"
    else
        AC_MSG_ERROR([invalid Firebird root directory: $ac_cv_firebird_root])
    fi
fi

# Check for custom includes path
if test [ -z "$ac_cv_firebird_includes" ]
then
    AC_ARG_WITH([firebird-includes],
                AS_HELP_STRING([--with-firebird-includes], [path to Firebird header files]),
                [ac_cv_firebird_includes=$withval])
fi
if test [ -n "$ac_cv_firebird_includes" ]
then
    AC_CACHE_CHECK([Firebird includes], [ac_cv_firebird_includes], [ac_cv_firebird_includes=""])
    FIREBIRD_CFLAGS="-I$ac_cv_firebird_includes"
fi

# Check for custom library path
if test [ -z "$ac_cv_firebird_libs" ]
then
    AC_ARG_WITH([firebird-libs],
                AS_HELP_STRING([--with-firebird-libs], [path to Firebird libraries]),
                [ac_cv_firebird_libs=$withval])
fi
if test [ -n "$ac_cv_firebird_libs" ]
then
    AC_CACHE_CHECK([Firebird libraries], [ac_cv_firebird_libs], [ac_cv_firebird_libs=""])
    FIREBIRD_LIBS="-ldl"
fi

# If some path is missing, try to autodetermine with fb_config
if test [ -z "$ac_cv_firebird_includes" -o -z "$ac_cv_firebird_libs" ]
then
    if test [ -z "$fbconfig" ]
    then
        AC_PATH_PROG(fbconfig,fb_config)
    fi
    if test [ -z "$fbconfig" ]
    then
        AC_MSG_ERROR([fb_config executable not found
********************************************************************************
ERROR: cannot find Firebird libraries. If you want to compile with Firebird
       support, you must either specify the Firebird installation prefix using
       --with-firebird=PATH, specify file locations explicitly using
       --with-firebird-includes and --with-firebird-libs options, or make sure
       path to fb_config is listed in your PATH environment variable. If you
       want to disable Firebird support, use --without-firebird option.
********************************************************************************
])
    else
        if test [ -z "$ac_cv_firebird_includes" ]
        then
            AC_MSG_CHECKING(Firebird C flags)
            FIREBIRD_CFLAGS=`${fbconfig} --cflags`
            AC_MSG_RESULT($FIREBIRD_CFLAGS)
        fi
        if test [ -z "$ac_cv_firebird_libs" ]
        then
            AC_MSG_CHECKING(Firebird linker flags)
            FIREBIRD_LIBS="-ldl"
            AC_MSG_RESULT($FIREBIRD_LIBS)
        fi
    fi
fi
])
