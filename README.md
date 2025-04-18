NetworkManager
==============

Networking that Just Works
--------------------------

NetworkManager attempts to keep an active network connection available at all
times.  The point of NetworkManager is to make networking configuration and
setup as painless and automatic as possible.  NetworkManager is intended to
replace default route, replace other routes, set IP addresses, and in general
configure networking as NM sees fit (with the possibility of manual override as
necessary).  In effect, the goal of NetworkManager is to make networking Just
Work with a minimum of user hassle, but still allow customization and a high
level of manual network control.  If you have special needs, we'd like to hear
about them, but understand that NetworkManager is not intended for every
use-case.

NetworkManager will attempt to keep every network device in the system up and
active, as long as the device is available for use (has a cable plugged in,
the killswitch isn't turned on, etc).  Network connections can be set to
'autoconnect', meaning that NetworkManager will make that connection active
whenever it and the hardware is available.

"Settings services" store lists of user- or administrator-defined "connections",
which contain all the settings and parameters required to connect to a specific
network.  NetworkManager will _never_ activate a connection that is not in this
list, or that the user has not directed NetworkManager to connect to.


How it works
------------

The NetworkManager daemon runs as a privileged service (since it must access
and control hardware), but provides a D-Bus interface on the system bus to
allow for fine-grained control of networking.  NetworkManager does not store
connections or settings, it is only the mechanism by which those connections
are selected and activated.

To store pre-defined network connections, two separate services, the "system
settings service" and the "user settings service" store connection information
and provide these to NetworkManager, also via D-Bus.  Each settings service
can determine how and where it persistently stores the connection information;
for example, the GNOME applet stores its configuration in GConf, and the system
settings service stores its config in distro-specific formats, or in a distro-
agnostic format, depending on user/administrator preference.

A variety of other system services are used by NetworkManager to provide
network functionality: wpa_supplicant for wireless connections and 802.1x
wired connections, pppd for PPP and mobile broadband connections, DHCP clients
for dynamic IP addressing, dnsmasq for proxy nameserver and DHCP server
functionality for internet connection sharing, and avahi-autoipd for IPv4
link-local addresses.  Most communication with these daemons occurs, again,
via D-Bus.


How to use it
-------------

Install NetworkManager with your distribution's package manager.

As NetworkManager is actually a daemon that runs in the background, you need to
use one of the many existing client programs to interact with it.

Terminal clients:
- `nmcli`: advanced command line client that gives you full control over all the
  aspects of NetworkManager, developed as part of the NetworkManager project.
- `nmtui`: text-based user interface (TUI) client. Also for the terminal, but
  interactive and more user friendly, also part of the NetworkManager project.
- [`nmstate`][1]: declarative network API and command line tool that uses
  NetworkManager as backend.
- Ansible: use the [network-role][2] in your playbooks

GUI clients
- `nm-connection-editor` and `nm-applet`: basic GUI interfaces developed by
  the NetworkManager project.
- GNOME shell: interacts with NetworkManager via its default settings panel
  `gnome-control-center`
- KDE Plasma: interacts with NetworkManager via its default settings panel
  and `plasma-nm`


Why doesn't my network Just Work?
---------------------------------

Driver problems are the #1 cause of why NetworkManager sometimes fails to
connect to wireless networks.  Often, the driver simply doesn't behave in a
consistent manner, or is just plain buggy.  NetworkManager supports _only_
those drivers that are shipped with the upstream Linux kernel, because only
those drivers can be easily fixed and debugged.  ndiswrapper, vendor binary
drivers, or other out-of-tree drivers may or may not work well with
NetworkManager, precisely because they have not been vetted and improved by the
open-source community, and because problems in these drivers usually cannot
be fixed.

Sometimes, command-line tools like 'iwconfig' will work, but NetworkManager will
fail.  This is again often due to buggy drivers, because these drivers simply
aren't expecting the dynamic requests that NetworkManager and wpa_supplicant
make.  Driver bugs should be filed in the bug tracker of the distribution being
run, since often distributions customize their kernel and drivers.

Sometimes, it really is NetworkManager's fault.  If you think that's
the case, please file a bug at:

https://gitlab.freedesktop.org/NetworkManager/NetworkManager/issues

Attaching NetworkManager debug logs from the journal (or wherever your
distribution directs syslog's 'daemon' facility output, as
/var/log/messages or /var/log/daemon.log) is often very helpful, and
(if you can get) a working wpa_supplicant config file helps
enormously.  See the logging section of file
contrib/fedora/rpm/NetworkManager.conf for how to enable debug logging
in NetworkManager.


Requirements
------------

NetworkManager requires:

- Linux kernel >= 5.6 for some ethtool options (pause, eee, ring)


Documentation
-------------

Updated documentation can be found at https://networkmanager.dev/docs

Users can consult the man pages. Most relevant pages for normal users are:
- NetworkManager daemon: [`NetworkManager (8)`][3], [`NetworkManager.conf (5)`][4]
- nmcli: [`nmcli (1)`][5], [`nmcli-examples (5)`][6], [`nm-settings-nmcli (5)`][7]
- nmtui: [`nmtui (1)`][8]


Get in touch
------------

To connect with the community, get help or get involved see the available
communication channels at https://networkmanager.dev/community/

Report bugs or feature request in our [issue tracker](https://gitlab.freedesktop.org/NetworkManager/NetworkManager/-/issues).
See [Report issues](https://gitlab.freedesktop.org/NetworkManager/NetworkManager/-/blob/main/CONTRIBUTING.md?ref_type=heads#report-issues)
for details about how to do it.


Contribute
----------

To get involved, see [CONTRIBUTING.md](CONTRIBUTING.md) to find different ways
to contribute.


License
-------

NetworkManager is free software under GPL-2.0-or-later and LGPL-2.1-or-later.
See [CONTRIBUTING.md#legal](CONTRIBUTING.md#legal) and
[RELICENSE.md](RELICENSE.md) for details.


[1]: https://nmstate.io/
[2]: https://linux-system-roles.github.io/network/
[3]: https://networkmanager.dev/docs/api/latest/NetworkManager.html
[4]: https://networkmanager.dev/docs/api/latest/NetworkManager.conf.html
[5]: https://networkmanager.dev/docs/api/latest/nmcli.html
[6]: https://networkmanager.dev/docs/api/latest/nmcli-examples.html
[7]: https://networkmanager.dev/docs/api/latest/nm-settings-nmcli.html
[8]: https://networkmanager.dev/docs/api/latest/nmtui.html
