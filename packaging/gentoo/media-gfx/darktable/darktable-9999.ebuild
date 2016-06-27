# Copyright 1999-2015 Gentoo Foundation
# Distributed under the terms of the GNU General Public License v2
# $Header: $

EAPI=5

inherit cmake-utils flag-o-matic toolchain-funcs gnome2-utils fdo-mime git-r3 pax-utils eutils

EGIT_REPO_URI="git://github.com/darktable-org/darktable.git"

DESCRIPTION="A virtual lighttable and darkroom for photographers"
HOMEPAGE="http://www.darktable.org/"

LICENSE="GPL-3"
SLOT="0"
IUSE="cmstest colord cpu_flags_x86_sse3 flickr geo gnome-keyring gphoto2 graphicsmagick jpeg2k kde
lua nls opencl openexr openmp pax_kernel +slideshow +squish web-services webp"

# sse3 support is required to build darktable
REQUIRED_USE="cpu_flags_x86_sse3"

CDEPEND="
	dev-db/sqlite:3
	dev-libs/glib:2
	dev-libs/libxml2:2
	gnome-base/librsvg:2
	media-gfx/exiv2:0=[xmp]
	media-libs/lcms:2
	media-libs/lensfun
	media-libs/libpng:0=
	media-libs/tiff:0
	net-misc/curl
	virtual/jpeg
	x11-libs/cairo
	x11-libs/gdk-pixbuf:2
	x11-libs/gtk+:2
	x11-libs/pango
	colord? ( x11-misc/colord:0= )
	flickr? ( media-libs/flickcurl )
	geo? ( net-libs/libsoup:2.4 )
	gnome-keyring? (
		app-crypt/libsecret
		dev-libs/json-glib
	)
	gphoto2? ( media-libs/libgphoto2:= )
	graphicsmagick? ( media-gfx/graphicsmagick )
	jpeg2k? ( media-libs/openjpeg:0 )
	lua? ( >=dev-lang/lua-5.2 )
	opencl? ( virtual/opencl )
	openexr? ( media-libs/openexr:0= )
	slideshow? (
		media-libs/libsdl
		virtual/glu
		virtual/opengl
	)
	web-services? ( dev-libs/json-glib )
	webp? ( media-libs/libwebp:0= )"
RDEPEND="${CDEPEND}
	x11-themes/gtk-engines:2
	kde? ( kde-base/kwalletd )"
DEPEND="${CDEPEND}
	dev-util/intltool
	virtual/pkgconfig
	nls? ( sys-devel/gettext )"

pkg_pretend() {
	if use openmp ; then
		tc-has-openmp || die "Please switch to an openmp compatible compiler"
	fi
}

src_prepare() {
	use cpu_flags_x86_sse3 && append-flags -msse3

	sed -e "s:\(/share/doc/\)darktable:\1${PF}:" \
		-e "s:\(\${CMAKE_INSTALL_DATAROOTDIR}/doc/\)darktable:\1${PF}:" \
		-e "s:LICENSE::" \
		-i doc/CMakeLists.txt || die

	cmake-utils_src_prepare
}

src_configure() {
	local mycmakeargs=(
		$(cmake-utils_use_build cmstest CMSTEST)
		$(cmake-utils_use_use colord COLORD)
		$(cmake-utils_use_use flickr FLICKR)
		$(cmake-utils_use_use geo GEO)
		$(cmake-utils_use_use gnome-keyring LIBSECRET)
		$(cmake-utils_use_use gphoto2 CAMERA_SUPPORT)
		$(cmake-utils_use_use graphicsmagick GRAPHICSMAGICK)
		$(cmake-utils_use_use jpeg2k OPENJPEG)
		$(cmake-utils_use_use kde KWALLET)
		$(cmake-utils_use_use lua LUA)
		$(cmake-utils_use_use nls NLS)
		$(cmake-utils_use_use opencl OPENCL)
		$(cmake-utils_use_use openexr OPENEXR)
		$(cmake-utils_use_use openmp OPENMP)
		$(cmake-utils_use_use squish SQUISH)
		$(cmake-utils_use_build slideshow SLIDESHOW)
		$(cmake-utils_use_use webp WEBP)
		-DUSE_GLIBJSON=$((use gnome-keyring || use web-services) && echo ON || echo OFF)
		-DUSE_GNOME_KEYRING=OFF
		-DCUSTOM_CFLAGS=ON
	)
	cmake-utils_src_configure
}

src_install() {
	cmake-utils_src_install

	if use pax_kernel && use opencl ; then
		pax-mark Cm "${ED}"/usr/bin/${PN} || die
		eqawarn "USE=pax_kernel is set meaning that ${PN} will be run"
		eqawarn "under a PaX enabled kernel. To do so, the ${PN} binary"
		eqawarn "must be modified and this *may* lead to breakage! If"
		eqawarn "you suspect that ${PN} is broken by this modification,"
		eqawarn "please open a bug."
	fi
}

pkg_preinst() {
	gnome2_icon_savelist
}

pkg_postinst() {
	gnome2_icon_cache_update
	fdo-mime_desktop_database_update
}

pkg_postrm() {
	gnome2_icon_cache_update
	fdo-mime_desktop_database_update
}
