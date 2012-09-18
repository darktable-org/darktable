# Copyright 1999-2012 Gentoo Foundation
# Distributed under the terms of the GNU General Public License v2

EAPI="4"
inherit cmake-utils eutils git-2

DESCRIPTION="A virtual lighttable and darkroom for photographers"
HOMEPAGE="http://www.darktable.org/"
EGIT_REPO_URI="git://github.com/darktable-org/darktable.git"

EGIT_BRANCH="master"
EGIT_COMMIT="master"

LICENSE="GPL-3"
SLOT="0"
KEYWORDS="~amd64 ~x86"

IUSE="gphoto2 openmp gnome-keyring kde"
RDEPEND="dev-db/sqlite:3
	dev-libs/libxml2:2
	gnome-base/libglade:2.0
	gnome-keyring? ( gnome-base/gnome-keyring )
	kde? ( kde-base/kwalletd )
	media-gfx/exiv2
	virtual/jpeg
	media-libs/lcms:2
	>=media-libs/lensfun-0.2.3
	gphoto2? ( media-libs/libgphoto2 )
	media-libs/libpng
	gnome-base/librsvg:2
	media-libs/openexr
	media-libs/tiff
	net-misc/curl
	net-libs/libsoup
	x11-libs/cairo
	x11-libs/gdk-pixbuf:2
	x11-libs/gtk+:2"
DEPEND="${RDEPEND}
	dev-util/pkgconfig
	openmp? ( >=sys-devel/gcc-4.4[openmp] )"

src_configure() {
	mycmakeargs=(
		"$(cmake-utils_use_use openmp OPENMP)"
		"$(cmake-utils_use_use gphoto2 CAMERA_SUPPORT)"
		"$(cmake-utils_use_use gnome-keyring GNOME_KEYRING)"
		"$(cmake-utils_use_use kde KWALLET)"
		"-DINSTALL_IOP_EXPERIMENTAL=ON"
		"-DINSTALL_IOP_LEGACY=ON" )
	cmake-utils_src_configure
}
