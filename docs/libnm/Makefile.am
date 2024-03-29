## Process this file with automake to produce Makefile.in
AUTOMAKE_OPTIONS = 1.6

check_local =

# The name of the module
DOC_MODULE=libnm

# The top-level SGML file.
DOC_MAIN_SGML_FILE=$(DOC_MODULE)-docs.xml

# The directory containing the source code. Relative to $(srcdir).
# gtk-doc will search all .c & .h files beneath here for inline comments
# documenting functions and macros.
DOC_SOURCE_DIR= \
	$(top_builddir)/src/libnm-core-public \
	$(top_srcdir)/src/libnm-core-public \
	$(top_builddir)/src/libnm-core-impl \
	$(top_srcdir)/src/libnm-core-impl \
	$(top_srcdir)/src/libnm-client-public \
	$(top_builddir)/src/libnm-client-public \
	$(top_srcdir)/src/libnm-client-impl \
	$(top_builddir)/src/libnm-client-impl \
	$(NULL)

# Extra options to supply to gtkdoc-scan.
SCAN_OPTIONS=--rebuild-types --rebuild-sections --ignore-decorators='NM_AVAILABLE_IN_\d+_\d+|NM_DEPRECATED_IN_\d+_\d+|NM_DEPRECATED_IN_\d+_\d+_FOR\(\)'

# Extra options to supply to gtkdoc-mkdb.
MKDB_OPTIONS=--sgml-mode --output-format=xml

# Extra options to supply to gtkdoc-fixref.
FIXXREF_OPTIONS=

# Used for dependencies.
HFILE_GLOB=$(top_srcdir)/src/libnm-core-public/*.h $(top_srcdir)/src/libnm-client-public/*.h
CFILE_GLOB=$(top_srcdir)/src/libnm-core-impl/*.c $(top_srcdir)/src/libnm-client-impl/*.c

# Header files to ignore when scanning.
IGNORE_HFILES= \
	nm-dbus-helpers.h \
	nm-default-libnm.h \
	nm-device-private.h \
	nm-dhcp4-config.h \
	nm-dhcp6-config.h \
	nm-dns-manager.h \
	nm-ip4-config.h \
	nm-ip6-config.h \
	nm-libnm-utils.h \
	nm-object-private.h \
	nm-remote-connection-private.h \
	\
	nm-connection-private.h \
	nm-default-libnm-core.h \
	nm-setting-private.h \
	nm-team-utils.h \
	nm-utils-private.h \
	\
	nm-core-tests-enum-types.h \
	test-general-enums.h \
	\
	$(NULL)

# Images to copy into HTML directory.
HTML_IMAGES = libnm.png

# Extra XML files that are included by $(DOC_MAIN_SGML_FILE).
content_files = version.xml

# Other files to distribute.
extra_files = libnm.png


# CFLAGS and LDFLAGS for compiling scan program. Only needed
# if $(DOC_MODULE).types is non-empty.
GTKDOC_CFLAGS = \
	-I$(top_builddir)/src/libnm-core-public \
	-I$(top_srcdir)/src/libnm-core-public \
	-I$(top_srcdir)/src/libnm-client-public \
	-I$(top_builddir)/src/libnm-client-public \
	-DNM_VERSION_MIN_REQUIRED=NM_VERSION_0_9_8 \
	$(GLIB_CFLAGS) \
	$(SANITIZER_EXEC_CFLAGS)

GTKDOC_LIBS = \
	$(top_builddir)/src/libnm-client-impl/libnm.la \
	$(GLIB_LIBS) \
	$(SANITIZER_EXEC_LDFLAGS)


# include common portion ...
include $(top_srcdir)/gtk-doc.make

EXTRA_DIST += \
	version.xml.in \
	meson.build \
	$(NULL)

CLEANFILES += \
	html/* \
	tmpl/* \
	xml/* \
	libnm-overrides.txt \
	libnm-sections.txt \
	libnm.actions \
	$(NULL)

if GTK_DOC_BUILD_HTML
check-local-gtk-doc-patch:
	@if grep -q -F '<a href="libnm-nm-setting-user.html">nm-setting-user</a>' "$(top_builddir)/docs/libnm/html/index.html"; then \
		echo "WARNING: The generated documentation has issues. Patch your gtk-doc (see https://gitlab.gnome.org/GNOME/gtk-doc/merge_requests/2). Let this check fail with NMTST_CHECK_GTK_DOC=1"; \
		test "$$NMTST_CHECK_GTK_DOC" != 1; \
	fi
check_local += check-local-gtk-doc-patch
endif

check-local: $(check_local)
