# Copyright 1999-2010 Gentoo Foundation
# Distributed under the terms of the GNU General Public License v3

EAPI="2"
GCONF_DEBUG="no"
inherit gnome2 cmake-utils

SRC_URI="http://downloads.sourceforge.net/project/darktable/darktable/0.8/darktable-0.8.tar.gz"
DESCRIPTION="Darktable is a virtual lighttable and darkroom for photographers"
HOMEPAGE="http://www.darktable.org/"

LICENSE="GPL-3"
SLOT="0"
KEYWORDS="amd64 x86"

IUSE="gconf gphoto openmp gnome-keyring"
RDEPEND="dev-db/sqlite:3
	dev-libs/libxml2:2
	gconf? ( gnome-base/gconf )
	gnome-base/libglade:2.0
	gnome-keyring? ( gnome-base/gnome-keyring )
	media-gfx/exiv2
	media-libs/jpeg
	media-libs/lcms
	>=media-libs/lensfun-0.2.3
	gphoto? ( media-libs/libgphoto2 )
	media-libs/libpng
	gnome-base/librsvg:2
	media-libs/openexr
	media-libs/tiff
	net-misc/curl
	x11-libs/cairo
	x11-libs/gtk+:2"
DEPEND="${RDEPEND}
	dev-util/pkgconfig
	openmp? ( >=sys-devel/gcc-4.4[openmp] )"

src_configure() {
	mycmakeargs=(
		"$(cmake-utils_use_use openmp OPENMP)"
		"$(cmake-utils_use_use gconf GCONF_BACKEND)"
		"$(cmake-utils_use_use gphoto CAMERA_SUPPORT)"
		"-DINSTALL_IOP_EXPERIMENTAL=ON"
		"-DINSTALL_IOP_LEGACY=ON" )
	cmake-utils_src_configure
}
