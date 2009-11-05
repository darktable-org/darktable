# Copyright 1999-2009 Gentoo Foundation
# Distributed under the terms of the GNU General Public License v2
# $Header: $

# ebuild for darktable by jo hanika (hanatos@gmail.com)
inherit eutils git

DESCRIPTION="Utility to organize and develop raw images"
HOMEPAGE="http://darktable.sf.net/"
EGIT_REPO_URI="git://darktable.git.sf.net/gitroot/darktable/darktable"
EGIT_BRANCH="master"
EGIT_TREE="master"
LICENSE="GPL-3"
SLOT="0"
KEYWORDS="~amd64"
IUSE="openmp"
DEPEND=">=x11-libs/gtk+-2.14.7 >=gnome-base/libglade-2.6.3
>=dev-db/sqlite-3.6.11 >=x11-libs/cairo-1.8.6
>=media-libs/gegl-0.0.22 >=media-libs/lcms-1.17 >=media-libs/jpeg-6b-r8
>=media-gfx/exiv2-0.18.1 >=media-libs/libpng-1.2.38 >=dev-util/intltool-0.40.5"
RDEPEND="${DEPEND}"

src_unpack() {
	git_src_unpack
	cd "${S}"
}

src_compile() {
	./autogen.sh
	econf $(use_enable openmp)
	emake || die "emake failed."
}

src_install() {
	emake DESTDIR="${D}" install || die "emake install failed."
	dodoc README TODO
}
