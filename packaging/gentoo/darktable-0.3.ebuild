# Copyright 1999-2009 Gentoo Foundation
# Distributed under the terms of the GNU General Public License v3
# $Header: $

# ebuild for darktable by jo hanika (hanatos@gmail.com)
inherit eutils

DESCRIPTION="Utility to organize and develop raw images"
HOMEPAGE="http://www.darktable.org/"
SRC_URI="http://switch.dl.sourceforge.net/sourceforge/darktable/${P}.tar.bz2"
LICENSE="GPL-3"
SLOT="0"
KEYWORDS="~amd64"
IUSE="openmp"
DEPEND=">=x11-libs/gtk+-2.14.7 >=gnome-base/libglade-2.6.3
>=dev-db/sqlite-3.6.11 >=x11-libs/cairo-1.8.6
>=media-libs/gegl-0.0.22 >=media-libs/lcms-1.17 >=media-libs/jpeg-6b-r8
>=media-gfx/exiv2-0.18.1 >=media-libs/libpng-1.2.38"
RDEPEND="${DEPEND}"

src_unpack() {
	unpack ${A}
	cd "${S}"
}

src_compile() {
	econf $(use_enable openmp)
	emake || die "emake failed."
}

src_install() {
	emake DESTDIR="${D}" install || die "emake install failed."
	dodoc README TODO
}
