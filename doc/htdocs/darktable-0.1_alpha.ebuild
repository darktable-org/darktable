# Copyright 1999-2009 Gentoo Foundation
# Distributed under the terms of the GNU General Public License v2
# $Header: $

inherit eutils

DESCRIPTION="Utility to organize and develop raw images"
HOMEPAGE="http://www.darktable.org/"
# TODO: keep to gentoo naming conventions (_alpha) for the next release..
P="darktable-0.1-alpha"
S="${WORKDIR}/${P}"
SRC_URI="http://switch.dl.sourceforge.net/sourceforge/darktable/${P}.tar.bz2"
LICENSE="GPL-3"
SLOT="0"
KEYWORDS="~x86 ~amd64"
IUSE=""
DEPEND=">=x11-libs/gtk+-2.14.7 >=gnome-base/libglade-2.6.3
>=media-gfx/imagemagick-6.4.8.3 >=dev-db/sqlite-3.6.11 >=x11-libs/cairo-1.8.6"
RDEPEND="${DEPEND}"

src_unpack() {
	unpack ${A}
	cd "${S}"
}

src_compile() {
	econf
	emake || die "emake failed."
}

src_install() {
	emake DESTDIR="${D}" install || die "emake install failed."
	dodoc README TODO
}
