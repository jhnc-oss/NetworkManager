AUTOMAKE_OPTIONS = 1.7

XSLTPROC = xsltproc --xinclude --nonet
XMLS = $(wildcard $(top_srcdir)/introspection/nm-*.xml)
GENERATED_FILES =

if BUILD_DOCS
GENERATED_FILES += settings-spec.xml
endif

# The name of the module.
DOC_MODULE=NetworkManager

# The top-level SGML file.
DOC_MAIN_SGML_FILE=network-manager-docs.xml

DOC_SOURCE_DIR=$(srcdir)

# Extra options to supply to gtkdoc-mkdb
MKDB_OPTIONS=--sgml-mode --output-format=xml

# Extra options to supply to gtkdoc-mktmpl
MKTMPL_OPTIONS=

# Extra options to supply to gtkdoc-mkhtml
MKHTML_OPTIONS=--path="$(abs_srcdir)"

# Files to be included in $(DOC_MAIN_SGML_FILE)
content_files = \
	$(GENERATED_FILES) \
	dbus-org.freedesktop.NetworkManager.AccessPoint.xml \
	dbus-org.freedesktop.NetworkManager.AgentManager.xml \
	dbus-org.freedesktop.NetworkManager.Connection.Active.xml \
	dbus-org.freedesktop.NetworkManager.DHCP4Config.xml \
	dbus-org.freedesktop.NetworkManager.DHCP6Config.xml \
	dbus-org.freedesktop.NetworkManager.Device.Adsl.xml \
	dbus-org.freedesktop.NetworkManager.Device.Bluetooth.xml \
	dbus-org.freedesktop.NetworkManager.Device.Bond.xml \
	dbus-org.freedesktop.NetworkManager.Device.Bridge.xml \
	dbus-org.freedesktop.NetworkManager.Device.Dummy.xml \
	dbus-org.freedesktop.NetworkManager.Device.Generic.xml \
	dbus-org.freedesktop.NetworkManager.Device.Hsr.xml \
	dbus-org.freedesktop.NetworkManager.Device.IPTunnel.xml \
	dbus-org.freedesktop.NetworkManager.Device.Infiniband.xml \
	dbus-org.freedesktop.NetworkManager.Device.Loopback.xml \
	dbus-org.freedesktop.NetworkManager.Device.Macsec.xml \
	dbus-org.freedesktop.NetworkManager.Device.Macvlan.xml \
	dbus-org.freedesktop.NetworkManager.Device.Modem.xml \
	dbus-org.freedesktop.NetworkManager.Device.OlpcMesh.xml \
	dbus-org.freedesktop.NetworkManager.Device.Statistics.xml \
	dbus-org.freedesktop.NetworkManager.Device.Team.xml \
	dbus-org.freedesktop.NetworkManager.Device.Tun.xml \
	dbus-org.freedesktop.NetworkManager.Device.Veth.xml \
	dbus-org.freedesktop.NetworkManager.Device.Vlan.xml \
	dbus-org.freedesktop.NetworkManager.Device.Vxlan.xml \
	dbus-org.freedesktop.NetworkManager.Device.WifiP2P.xml \
	dbus-org.freedesktop.NetworkManager.Device.WireGuard.xml \
	dbus-org.freedesktop.NetworkManager.Device.Wired.xml \
	dbus-org.freedesktop.NetworkManager.Device.Wireless.xml \
	dbus-org.freedesktop.NetworkManager.Device.xml \
	dbus-org.freedesktop.NetworkManager.DnsManager.xml \
	dbus-org.freedesktop.NetworkManager.IP4Config.xml \
	dbus-org.freedesktop.NetworkManager.IP6Config.xml \
	dbus-org.freedesktop.NetworkManager.PPP.xml \
	dbus-org.freedesktop.NetworkManager.SecretAgent.xml \
	dbus-org.freedesktop.NetworkManager.Settings.Connection.xml \
	dbus-org.freedesktop.NetworkManager.Settings.xml \
	dbus-org.freedesktop.NetworkManager.VPN.Connection.xml \
	dbus-org.freedesktop.NetworkManager.VPN.Plugin.xml \
	dbus-org.freedesktop.NetworkManager.xml \
	$(top_builddir)/src/libnm-core-public/nm-dbus-types.xml \
	$(top_builddir)/src/libnm-core-public/nm-vpn-dbus-types.xml \
	$(top_builddir)/man/nmcli.xml \
	$(top_builddir)/man/nmtui.xml \
	$(top_builddir)/man/nm-online.xml \
	$(top_builddir)/man/nm-initrd-generator.xml \
	$(top_builddir)/man/NetworkManager.xml \
	$(top_builddir)/man/NetworkManager-dispatcher.xml \
	$(top_builddir)/man/NetworkManager.conf.xml \
	$(top_builddir)/man/NetworkManager-wait-online.service.xml \
	$(top_builddir)/man/nmcli-examples.xml \
	$(top_builddir)/man/nm-settings-dbus.xml \
	$(top_builddir)/man/nm-settings-keyfile.xml \
	$(top_builddir)/man/nm-settings-nmcli.xml \
	version.xml \
	$(NULL)


if WITH_OPENVSWITCH
content_files += $(top_builddir)/man/nm-openvswitch.xml
endif

if BUILD_NM_CLOUD_SETUP
content_files += $(top_builddir)/man/nm-cloud-setup.xml
endif

if CONFIG_PLUGIN_IFCFG_RH
content_files += $(top_builddir)/man/nm-settings-ifcfg-rh.xml
endif

include $(top_srcdir)/gtk-doc.make

# workaround setup-build.stamp from gtk-doc.make, which copies
# the file as read-only. That breaks later build steps, as the
# file can no longer be written. Instead, copy the file with `cp -f`
# only.
setup-build.stamp:
	-$(GTK_DOC_V_SETUP)if test "$(abs_srcdir)" != "$(abs_builddir)" ; then \
	  files=`echo $(SETUP_FILES) $(DOC_MODULE).types`; \
	  if test "x$$files" != "x" ; then \
	    for file in $$files ; do \
	      destdir=`dirname $(abs_builddir)/$$file`; \
	      test -d "$$destdir" || mkdir -p "$$destdir"; \
	      test -f $(abs_srcdir)/$$file && \
	        ( cp -pf $(abs_srcdir)/$$file $(abs_builddir)/$$file ; \
	        chmod u+w $(abs_builddir)/$$file ) || true; \
	    done; \
	  fi; \
	fi
	$(AM_V_at)touch setup-build.stamp

settings-spec.xml: settings-spec.xsl $(top_builddir)/man/nm-settings-docs-dbus.xml
	$(AM_V_GEN) (! test -f $@ || chmod u+w $@) && xsltproc --output $@ $^

EXTRA_DIST += \
	version.xml.in \
	settings-spec.xsl \
	meson.build \
	$(GENERATED_FILES) \
	$(NULL)

CLEANFILES += \
	html/* \
	tmpl/* \
	xml/* \
	NetworkManager-overrides.txt \
	NetworkManager-sections.txt \
	NetworkManager.actions \
	settings-spec.xml \
	$(NULL)
