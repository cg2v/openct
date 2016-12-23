MAINTAINERCLEANFILES = $(srcdir)/Makefile.in

if ENABLE_PCSC
lib_LTLIBRARIES = openct-ifd.la
noinst_PROGRAMS = test
endif

openct_ifd_la_SOURCES = pcsc-ifdhandler.c
openct_ifd_la_LDFLAGS = -module -shared -avoid-version -no-undefined
openct_ifd_la_LIBADD = $(PCSC_LIBS) $(top_builddir)/src/ifd/libifd.la
openct_ifd_la_CFLAGS = $(AM_CFLAGS) $(PCSC_CFLAGS) \
	-I$(top_srcdir)/src/include \
	-I$(top_builddir)/src/include \
	-I$(top_srcdir)/src/ifd

test_SOURCES = test.c
test_LDADD = openct-ifd.la
test_CFLAGS = $(AM_CFLAGS) $(PCSC_CFLAGS)


# Don't use ENABLE_BUNDLES for this case, pcsc-lite loads
# USB drivers from MacOS X like bundles for all platforms
install-exec-local: install-libLTLIBRARIES
if ENABLE_PCSC
if ENABLE_BUNDLES
	$(MKDIR_P) "$(DESTDIR)$(bundledir)"
	"$(top_srcdir)/macos/libtool-bundle" -e "$(top_srcdir)/etc/Info.plist" \
		"$(DESTDIR)$(libdir)/openct-ifd.so" "$(DESTDIR)$(bundledir)"
endif
endif

uninstall-local: uninstall-libLTLIBRARIES
if ENABLE_PCSC
if ENABLE_BUNDLES
	rm -rf "$(DESTDIR)$(bundledir)/openct-ifd.bundle/"
endif
endif
