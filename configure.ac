AC_INIT([sc2mpd], [0.14.0], [jf@dockes.org],
             [sc2mpd], [http://www.lesbonscomptes.com/upmpdcli])
AC_PREREQ([2.53])
AC_CONFIG_SRCDIR([sc2src/sc2mpd.cpp])
AC_CONFIG_HEADERS([sc2src/config.h])

AM_INIT_AUTOMAKE([1.10 no-define subdir-objects foreign])

AC_PROG_CXX
AC_C_BIGENDIAN

AC_ARG_WITH(openhome, 
    AC_HELP_STRING([--with-openhome=topOpenhomeDir],
   [Set the path for the directory where ohNet, ohNetGenerated, ohdevtools,
    ohTopology and ohSongcast were extracted and built]),
        withOH=$withval, withOH=no)

if test X$withOH = Xno ; then
   AC_MSG_ERROR([Please provide the location for the OpenHome sources and libraries])
fi
TOPOH="$withOH"
AC_SUBST(TOPOH)                     

AC_CHECK_HEADERS([byteswap.h])

AC_CHECK_LIB([pthread], [pthread_create], , [lpthread=no])
if test X$lpthread = Xno; then
   AC_MSG_ERROR([pthread_create not found in -lpthread])
fi
AC_CHECK_LIB([microhttpd], [MHD_start_daemon], , [lmh=no])
if test X$lmh = Xno; then
   AC_MSG_ERROR([libmicrohttpd development files not found])
fi
AC_CHECK_LIB([samplerate], [src_process], , [lsrc=no])
if test X$lsrc = Xno; then
   AC_MSG_ERROR([libsamplerate development files not found])
fi
AC_CHECK_LIB([asound], [snd_pcm_open], , [lsnd=no])
if test X$lsnd = Xno; then
   AC_MSG_ERROR([libasound development files not found])
fi

OTHERLIBS=$LIBS
echo OTHERLIBS $OTHERLIBS
AC_SUBST(OTHERLIBS)

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
