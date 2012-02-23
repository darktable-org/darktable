#
# spec file for package darktable (Version 0.8)
#
# Copyright (c) 2011 Ulrich Pegelow
# This file and all modifications and additions to the pristine
# package are under the same license as the package itself.
#

# norootforbuild

Name:		darktable
Version:	0.8
Release:	1.1
License:	GPLv3+
Summary:	Raw Photo Viewer and Organiser
URL:		http://www.darktable.org/
Group:		Productivity/Graphics/Viewers
Source0:	%{name}-%{version}.tar.gz
BuildRequires:	gcc-c++ libglade2-devel libtiff-devel cairo-devel libexiv2-devel
BuildRequires:	sqlite3-devel lensfun-devel liblcms-devel
BuildRequires:	intltool update-desktop-files fdupes
BuildRoot:	%{_tmppath}/%{name}-%{version}-build

%description
darktable is a virtual lighttable and darkroom for photographers: it manages
your digital negatives in a database and lets you view them through a zoomable
lighttable. it also enables you to develop raw images and enhance them.

Authors:
--------
    Alexander Rabtchevich
    Alexandre Prokoudine
    Christian Himpel
    Gregor Quade
    Henrik Andersson
    Johannes Hanika
    Pascal de Bruijn
    Richard Hughes
    Sébastien Delcoigne
    Thomas Costis


%define prefix   /usr

%prep

%setup -q

%build

export CXXFLAGS="$RPM_OPT_FLAGS"  
export CFLAGS="$CXXFLAGS" 

mkdir -p build
cd build

cmake -DCMAKE_INSTALL_PREFIX=%prefix -DCMAKE_BUILD_TYPE=Release \
	-DINSTALL_IOP_EXPERIMENTAL=OFF -DINSTALL_IOP_LEGACY=ON ..

make %{?jobs:-j %jobs} 

%install

cd build
make install DESTDIR="$RPM_BUILD_ROOT"

mkdir -p %{buildroot}/usr/share/doc/packages
%{__mv} %{buildroot}%{_datadir}/doc/%{name} %{buildroot}/usr/share/doc/packages
%find_lang %{name}
%suse_update_desktop_file -i %{name}
%fdupes -s %{buildroot}%{_datadir}



%clean
[ "%{buildroot}" != "/" ] && %{__rm} -rf %{buildroot}

%pre -f build/%{name}.schemas_pre

%preun -f build/%{name}.schemas_preun

%posttrans -f build/%{name}.schemas_posttrans

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%files -f build/%{name}.lang -f build/%{name}.schemas_list
%defattr(-,root,root)
%doc %{_defaultdocdir}/%{name}
%{_bindir}/*
%{_datadir}/%{name}
%{_libdir}/%{name}
%{_mandir}/man1/*
%{_datadir}/applications/%{name}.desktop
/usr/share/icons/*/*/apps/darktable.*

%changelog
* Sat May 14 2011 - ulrich.pegelow@tongareva.de
- Initial release
