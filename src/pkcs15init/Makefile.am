include $(top_srcdir)/win32/ltrc.inc

MAINTAINERCLEANFILES = $(srcdir)/Makefile.in
EXTRA_DIST = Makefile.mak

noinst_LTLIBRARIES = libpkcs15init.la
noinst_HEADERS = profile.h pkcs15-init.h pkcs15-oberthur.h pkcs15-iasecc.h
dist_pkgdata_DATA = \
	cyberflex.profile \
	flex.profile \
	cardos.profile \
	oberthur.profile \
	starcos.profile \
	setcos.profile \
	pkcs15.profile \
	muscle.profile \
	rutoken.profile \
	asepcos.profile \
	entersafe.profile \
	epass2003.profile \
	rutoken_ecp.profile \
	rutoken_lite.profile \
	myeid.profile \
	authentic.profile \
	iasecc.profile \
	ias_adele_admin1.profile ias_adele_admin2.profile ias_adele_common.profile \
	iasecc_generic_pki.profile iasecc_admin_eid.profile iasecc_generic_oberthur.profile \
	jacartapki.profile \
	openpgp.profile sc-hsm.profile \
	isoApplet.profile gids.profile

AM_CPPFLAGS = -D'SC_PKCS15_PROFILE_DIRECTORY="$(pkgdatadir)"' \
	-I$(top_srcdir)/src
AM_CFLAGS = $(OPTIONAL_OPENSSL_CFLAGS)

libpkcs15init_la_SOURCES = \
	pkcs15-lib.c profile.c \
	pkcs15-cflex.c \
	pkcs15-cardos.c pkcs15-starcos.c \
	pkcs15-setcos.c pkcs15-muscle.c \
	pkcs15-asepcos.c pkcs15-rutoken.c \
	pkcs15-entersafe.c pkcs15-epass2003.c \
	pkcs15-rtecp.c pkcs15-myeid.c \
	pkcs15-oberthur.c pkcs15-oberthur-awp.c \
	pkcs15-authentic.c pkcs15-iasecc.c pkcs15-jacartapki.c pkcs15-openpgp.c \
	pkcs15-sc-hsm.c \
	pkcs15-isoApplet.c pkcs15-gids.c
