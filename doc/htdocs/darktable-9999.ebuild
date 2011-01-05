# Copyright 1999-2010 Gentoo Foundation
# Distributed under the terms of the GNU General Public License v2

EAPI="2"
GCONF_DEBUG="no"
inherit gnome2 cmake-utils git
unset SRC_URI

DESCRIPTION="Darktable is a virtual lighttable and darkroom for photographers"
HOMEPAGE="http://darktable.sf.net/"
EGIT_REPO_URI="git://darktable.git.sf.net/gitroot/darktable/darktable"

EGIT_BRANCH="master"
EGIT_COMMIT="master"

LICENSE="GPL-3"
SLOT="0"
KEYWORDS="~amd64 ~x86"

IUSE="dbus gconf gphoto openmp gnome-keyring doc"
RDEPEND="dev-db/sqlite:3
	doc? ( dev-java/fop )
	dbus? ( dev-libs/dbus-glib )
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
	media-libs/libraw
	gnome-base/librsvg:2
	media-libs/openexr
	media-libs/tiff
	net-misc/curl
	x11-libs/cairo
	x11-libs/gdk-pixbuf:2
	x11-libs/gtk+:2"
DEPEND="${RDEPEND}
	dev-util/pkgconfig
	openmp? ( >=sys-devel/gcc-4.4[openmp] )"

src_configure() {
	mycmakeargs=(
		"$(cmake-utils_use_use openmp OPENMP)"
		"$(cmake-utils_use_use gconf GCONF_BACKEND)"
		"$(cmake-utils_use_use gphoto CAMERA_SUPPORT)"
		"-DDONT_INSTALL_GCONF_SCHEMAS=ON" )
	cmake-utils_src_configure
}
