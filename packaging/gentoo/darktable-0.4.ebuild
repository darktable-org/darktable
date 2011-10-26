# Copyright 1999-2009 Gentoo Foundation
# Distributed under the terms of the GNU General Public License v2
# $Header: $

# ebuild for darktable by jo hanika (hanatos@gmail.com)
inherit eutils git

DESCRIPTION="Utility to organize and develop raw images"
HOMEPAGE="http://www.darktable.org/"
SRC_URI="http://switch.dl.sourceforge.net/sourceforge/darktable/${P}.tar.bz2"
LICENSE="GPL-3"
SLOT="0"
KEYWORDS="~amd64"
IUSE="openmp"
DEPEND=">=x11-libs/gtk+-2.18.0 >=gnome-base/libglade-2.6.3
>=dev-db/sqlite-3.6.11 >=x11-libs/cairo-1.8.6
>=media-libs/gegl-0.0.22 >=media-libs/lcms-1.17 >=media-libs/jpeg-6b-r8
>=media-gfx/exiv2-0.18.1 >=media-libs/libpng-1.2.38 >=dev-util/intltool-0.40.5
>=media-libs/lensfun-0.2.4 >=gnome-base/gconf-2.24.0"
RDEPEND="${DEPEND}"

src_unpack() {
	unpack ${A}
	cd "${S}"
}

src_compile() {
	./autogen.sh
	econf $(use_enable openmp) --disable-schemas-install
	emake || die "emake failed."
}

src_install() {
	emake DESTDIR="${D}" install || die "emake install failed."
	dodoc README TODO
}

pkg_postinst() {
	export GCONF_CONFIG_SOURCE=xml::/etc/gconf/gconf.xml.defaults
	einfo "Installing darktable GConf schemas"
	${ROOT}/usr/bin/gconftool-2 --makefile-install-rule ${S}/darktable.schemas 1>/dev/null
}

pkg_postrm() {
	export GCONF_CONFIG_SOURCE=xml::/etc/gconf/gconf.xml.defaults
	einfo "Uninstalling darktable GConf schemas"
	${ROOT}/usr/bin/gconftool-2 --makefile-uninstall-rule darktable.schemas 1>/dev/null
}

