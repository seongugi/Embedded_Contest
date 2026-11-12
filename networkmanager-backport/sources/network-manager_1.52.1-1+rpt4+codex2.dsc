Format: 3.0 (quilt)
Source: network-manager
Binary: network-manager, network-manager-dev, network-manager-l10n, libnm0, libnm-dev, gir1.2-nm-1.0, network-manager-config-connectivity-debian
Architecture: linux-any all
Version: 1.52.1-1+rpt4+codex2
Maintainer: Utopia Maintenance Team <pkg-utopia-maintainers@lists.alioth.debian.org>
Uploaders: Michael Biebl <biebl@debian.org>
Homepage: https://networkmanager.dev/
Standards-Version: 4.7.2
Vcs-Browser: https://salsa.debian.org/utopia-team/network-manager
Vcs-Git: https://salsa.debian.org/utopia-team/network-manager.git
Testsuite: autopkgtest
Testsuite-Triggers: dnsmasq-base, python3, python3-dbusmock, udev
Build-Depends: debhelper-compat (= 13), debhelper (>= 13.11.6), dh-sequence-gir, meson (>= 0.51.0), pkgconf, gettext (>= 0.19.8), libglib2.0-dev (>= 2.42), ppp-dev, libselinux1-dev, libaudit-dev, libgnutls28-dev (>= 2.12), uuid-dev, systemd-dev, libsystemd-dev (>= 209), libudev-dev (>= 175), gir1.2-gio-2.0-dev, gir1.2-girepository-2.0, gobject-introspection, python3-gi, libpsl-dev (>= 0.1), libcurl4-gnutls-dev (>= 7.24.0), gtk-doc-tools, libglib2.0-doc, libmm-glib-dev (>= 0.7.991), libndp-dev, libreadline-dev, libnewt-dev (>= 0.52.15), libteam-dev (>= 1.9), libjansson-dev (>= 2.7), libbluetooth-dev (>= 5), libdbus-1-dev, libpolkit-gobject-1-dev, mobile-broadband-provider-info, valac, polkitd, dbus <!nocheck>, python3-dbus <!nocheck>, python3-pexpect <!nocheck>, iproute2 <!nocheck>, libyaml-dev [!i386], libnetplan-dev (>= 0.106~) [!i386], netplan.io (>= 0.106~) [!i386] <!nocheck>
Package-List:
 gir1.2-nm-1.0 deb introspection optional arch=linux-any
 libnm-dev deb libdevel optional arch=linux-any
 libnm0 deb libs optional arch=linux-any
 network-manager deb net optional arch=linux-any
 network-manager-config-connectivity-debian deb net optional arch=all
 network-manager-dev deb devel optional arch=all
 network-manager-l10n deb localization optional arch=all
Checksums-Sha1:
 a0fb4021a68011735b8c1a55657b2f4f443b75e8 5541480 network-manager_1.52.1.orig.tar.xz
 a46823a2eb89e34e04f7fb2ce19169e9706122b3 71200 network-manager_1.52.1-1+rpt4+codex2.debian.tar.xz
Checksums-Sha256:
 178ddd8d5ee70c24f598db3aa237c248a2eb6f9e831b8f3e9fdff793a0f11def 5541480 network-manager_1.52.1.orig.tar.xz
 cb075841ccfc151cbdf0081315f7c080fdc26aa08d2f6ed7c8d4cc93e59faf0a 71200 network-manager_1.52.1-1+rpt4+codex2.debian.tar.xz
Files:
 3bf7d7507f539c1131a0c1a0fc8add04 5541480 network-manager_1.52.1.orig.tar.xz
 abb5bd24b1892ea93adfaf29969825c9 71200 network-manager_1.52.1-1+rpt4+codex2.debian.tar.xz
