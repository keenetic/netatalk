dnl Autoconf macro to check for libndm support

AC_DEFUN([AC_NETATALK_CHECK_LIBNDM], [
	AC_ARG_ENABLE(libndm,
	[  --enable-libndm           Support NDM core control (default=no)])

	if test "x$enable_libndm" = "xyes"; then
		AC_CHECK_HEADERS(ndm/core.h)
		AC_CHECK_LIB([ndm], [ndm_core_open])
		AC_DEFINE(WITH_LIBNDM,,[with libndm])
	fi
])

