bin_PROGRAMS = unix_srp

unix_srp_SOURCES = \
    unix_srp.c

unix_srp_CPPFLAGS = \
   -I$(top_srcdir)/vmdir/include \
   -I$(top_srcdir)/vmdir \
   -I$(top_srcdir)/vmdir/include/public \
   -I$(top_srcdir)/vmdir/thirdparty \
   -I$(top_builddir)/vmdir/client \
    @OPENSSL_INCLUDES@ \
    -Wno-pointer-sign

unix_srp_LDADD = \
    $(top_builddir)/vmdir/thirdparty/csrp/libcsrp.la \
    $(top_builddir)/vmdir/thirdparty/csrp/libcsrp.la \
    @CRYPT_LIBS@ \
    @OPENSSL_LDFLAGS@ \
    @CRYPTO_LIBS@

lib_LTLIBRARIES = libgssapi_unix.la

libgssapi_unix_la_CPPFLAGS = \
   -D_MIT_KRB5_1_11 \
   -D_MIT_KRB5_1_12 \
   -I. \
   -I$(top_srcdir)/vmdir/gssapi-plugins/srp \
   -I$(top_srcdir)/vmdir/include \
   -I$(top_srcdir)/vmdir \
   -I$(top_srcdir)/vmdir/include/public \
   -I$(top_srcdir)/vmdir/thirdparty \
   -I$(top_builddir)/vmdir/client \
    @OPENSSL_INCLUDES@ \
    @LW_INCLUDES@ \
    -Wno-pointer-sign

libgssapi_unix_la_SOURCES = \
        gssapi_alloc.c \
	unix_accept_sec_ctx.c \
	unix_acquire_cred.c \
	unix_release_cred.c \
	unix_disp_name.c \
	unix_encrypt.c \
	unix_init_sec_ctx.c \
	unix_del_sec_ctx.c \
	unix_mech.c \
	unix_mech_desc.c \
	unix_wrap_iov.c \
	unix_unwrap_iov.c \
	unix_util.c \
        unixregutils.c \
        unixreg.c \
        unix_crypt.c

libgssapi_unix_la_LIBADD = \
    $(top_builddir)/vmdir/client/libvmdirclient.la \
    $(top_builddir)/vmdir/thirdparty/csrp/libcsrp.la \
    $(top_builddir)/vmdir/client/libvmdirclient_la-srp_verifier_cstub.lo \
    @DCERPC_LIBS@ \
    @GSSAPI_LIBS@ \
    @LDAP_LIBS@ \
    @OPENSSL_LDFLAGS@ \
    @CRYPT_LIBS@ \
    @PTHREAD_LIBS@

libgssapi_unix_la_LDFLAGS = \
    @LW_LDFLAGS@
