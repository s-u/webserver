# Process this file with autoconf to produce a configure script.
AC_INIT([webserver],[1.0],[Simon.Urbanek@r-project.org])
AC_CONFIG_SRCDIR([src/http.h])
AC_CONFIG_HEADERS([src/config.h])
AC_CONFIG_AUX_DIR([tools])
AC_CONFIG_MACRO_DIRS([tools/m4])

AC_ARG_ENABLE([ipv6],
[AS_HELP_STRING([--enable-ipv6],[enable the use of IPv6 protocol. @<:@no@:>@])],
[want_ipv6="${enableval}"],
[want_ipv6=no])

AC_ARG_ENABLE([tls],
[AS_HELP_STRING([--enable-tls],[enable the use of secure sockets using TLS/SSL. (default is @<:@auto@:>@ which means enabled is OpenSSL is present)])],
[want_tls="${enableval}"],
[want_tls=auto])

AC_ARG_ENABLE([threads],
[AS_HELP_STRING([--enable-threads],[enable the use of threads. @<:@yes@:>@])],
[want_threads="${enableval}"],
[want_threads=yes])

# find R home and set CC/CFLAGS
: ${R_HOME=`R RHOME`}
if test -z "${R_HOME}"; then
  echo "could not determine R_HOME"
  exit 1
fi

RBIN="${R_HOME}/bin/R"

# pick all flags for testing from R
: ${CC=`"${RBIN}" CMD config CC`}
: ${CFLAGS=`"${RBIN}" CMD config CFLAGS`}
: ${CPPFLAGS=`"${RBIN}" CMD config CPPFLAGS`}
: ${LDFLAGS=`"${RBIN}" CMD config LDFLAGS`}
: ${CPP="$CC -E"}
: ${PKGCONFIG=pkg-config}

# honor PKG_xx overrides
LIBS="${LIBS} ${PKG_LIBS}"
# for CPPFLAGS we will superfluously double R's flags
# since we'll set PKG_CPPFLAGS with this, but that shouldn't hurt
CPPFLAGS="${CPPFLAGS} ${PKG_CPPFLAGS}"

AC_SUBST(R_HOME)

AC_ARG_VAR([OPENSSL_INCLUDES],[optional path to the include directory for OpenSSL headers])
AC_ARG_VAR([PKG_CPPFLAGS],[additional pre-processor flags])
AC_ARG_VAR([PKG_LIBS],[additional linker library flags])
AC_ARG_VAR([PKGCONFIG],[command to use for flags detection if none are supplied, defaults to pkg-config])

# Checks for programs.
AC_PROG_CC

AC_CHECK_HEADERS([limits.h string.h memory.h sys/time.h unistd.h])
AC_CHECK_HEADERS([sys/stat.h sys/types.h sys/socket.h sys/param.h sys/un.h netinet/in.h netinet/tcp.h])

# connect may need -lsocket and/or -lnsl (e.g. on Solaris)
AC_CHECK_FUNCS(connect)
if test x"$ac_cv_func_connect" = x"no"; then
    case "$LIBS" in
    *-lnsl*) ;;
    *) AC_CHECK_LIB(nsl_s, printf) ;;
    esac
    case "$LIBS" in
    *-lnsl*) ;;
    *) AC_CHECK_LIB(nsl, printf) ;;
    esac
    case "$LIBS" in
    *-lsocket*) ;;
    *) AC_CHECK_LIB(socket, connect) ;;
    esac
    case "$LIBS" in
    *-linet*) ;;
    *) AC_CHECK_LIB(inet, connect) ;;
    esac
fi

# IPv6
have_ipv6=no
AC_MSG_CHECKING([whether to enable IPv6])
if test "$want_ipv6" = yes; then
AC_MSG_RESULT([yes])
AC_MSG_CHECKING([for working IPv6 sockets])
AC_LINK_IFELSE([AC_LANG_SOURCE([
#include <sys/socket.h>
#include <netinet/in.h>
int main(void) {
    struct sockaddr_in6 sin;
    int i = socket(PF_INET6, SOCK_STREAM, 0);
    sin.sin6_family = AF_INET6;
    sin.sin6_addr = in6addr_any;
    return 0;
}
])],[AC_MSG_RESULT(yes)
have_ipv6=yes],[AC_MSG_RESULT(no)])
else
AC_MSG_RESULT([no])
fi
if test "$have_ipv6" = yes; then
   AC_DEFINE(HAVE_IPV6, 1, [IPv6 support present and enabled])
fi

working_tls=no
## skip SSL tests is if was disabled
if test "x${want_tls}" != xno; then
   if test "x${OPENSSL_INCLUDES}" != x; then
      CPPFLAGS="${CPPFLAGS} -I${OPENSSL_INCLUDES}"
   else
      if test x"${PKG_CPPFLAGS}" = x -a x"${PKG_LIBS}" = x -a x"$PKGCONFIG" != x; then
         AC_MSG_CHECKING([for $PKGCONFIG])
	 if $PKGCONFIG --version >/dev/null 2>&1; then
            AC_MSG_RESULT(yes)
	    AC_MSG_CHECKING([whether it knows about openssl])
	    if $PKGCONFIG openssl; then
	       AC_MSG_RESULT(yes)
	       PC_CFLAGS=`$PKGCONFIG --cflags openssl`
	       PC_LIBS=`$PKGCONFIG --libs openssl`
	       AC_MSG_NOTICE([CFLAGS: ${PC_CFLAGS}])
	       AC_MSG_NOTICE([LIBS: ${PC_LIBS}])
	       CPPFLAGS="${CPPFLAGS} ${PC_CFLAGS}"
	       LIBS="${LIBS} ${PC_LIBS}"
	    else
	       AC_MSG_RESULT(no)
	    fi
	 else
            AC_MSG_RESULT(no)
	 fi
      fi
   fi

   # check SSL support
   AC_CHECK_HEADER([openssl/ssl.h],
      [AC_SEARCH_LIBS(SSL_CTX_load_verify_locations, [ssl openssl],
        [AC_SEARCH_LIBS(EVP_Digest, [crypto], [working_tls=yes])]
      )])
fi

if test "x${want_tls}" = xauto; then
   want_tls=${working_tls}
fi

if test "x${want_tls}${working_tls}" = xyesno; then
   AC_MSG_ERROR([--enable-tls was specified, but no working OpenSSL library was found.
Install libssl-dev or equivalent.])
fi

AC_MSG_CHECKING([whether TLS/SSL support is enabled])
AC_MSG_RESULT([${working_tls}])
if test "x${working_tls}"; then
   AC_DEFINE(HAVE_TLS, 1, [If defined TLS/SSL support is enabled.])
fi

## we have removed WebSockets code from this version, so it is always disabled for now
AC_DEFINE(NO_WEBSOCKETS, 1, [set if WebSocket support should be removed])

# check threads
AC_MSG_CHECKING([whether threads are desired])
if test "$want_threads" != no; then
   AC_MSG_RESULT([yes])
   AX_PTHREAD([
     LIBS="$PTHREAD_LIBS $LIBS"
     ## we don't want to mess with CFLAGS so we (ab)use CPPFLAGS
     CPPFLAGS="$CPPFLAGS $PTHREAD_CFLAGS" 
     ## and we ignore PTHREAD_CC since we have to use R's settings
     with_threads=yes
     AC_MSG_CHECKING([for working threads support])
     AC_MSG_RESULT([yes])
   ], [
     AC_MSG_CHECKING([for working threads support])
     AC_MSG_RESULT([no])
     if test "$want_threads" = yes; then
       AC_MSG_ERROR([Threads were requested, but no working threads support was found])
     fi 
     with_threads=no
   ])
else
   AC_MSG_RESULT([no])
   with_threads=no
fi

if test x"$with_threads" = xyes; then
   AC_DEFINE(WITH_THREADS, 1, [set if threads can be used])
fi

AC_SUBST(LIBS)
AC_SUBST(CPPFLAGS)

AC_CONFIG_FILES([src/Makevars])
AC_OUTPUT
