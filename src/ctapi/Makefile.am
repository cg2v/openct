MAINTAINERCLEANFILES = $(srcdir)/Makefile.in

if ENABLE_SERVER
openctinclude_HEADERS = ctapi.h
lib_LTLIBRARIES = libopenctapi.la
endif

libopenctapi_la_SOURCES = ctapi.c ctapi.h
libopenctapi_la_LIBADD = $(top_builddir)/src/ct/libopenct.la
libopenctapi_la_CFLAGS = $(AM_CFLAGS) \
	-I$(top_srcdir)/src/include \
	-I$(top_builddir)/src/include
libopenctapi_la_LDFLAGS = -avoid-version -no-undefined -module -shared
