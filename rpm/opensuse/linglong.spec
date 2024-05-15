Name:           linglong
Version:        1.5.0
Release:        1
Summary:        Linglong Package FrameWork 
License:        LGPLv3
URL:            https://github.com/linuxdeepin/%{name}
Source0:        %{url}/archive/%{version}/linglong-%{version}.tar.gz

BuildRequires:  cmake gcc12-c++
BuildRequires:  (qt5-qtbase-devel or libqt5-qtbase-devel) (qt5-qtwebsockets-devel or libqt5-qtwebsockets-devel) (qt5-qtbase-private-devel or libqt5-qtbase-private-headers-devel) 
BuildRequires:  glib2-devel (nlohmann-json-devel or nlohmann_json-devel) ostree-devel yaml-cpp-devel
BuildRequires:  systemd-devel gtest libseccomp-devel
Requires:       linglong-bin = %{version}-%{release}

%description
This package is a linglong package framework.

%package        -n linglong-bin
Summary:        Linglong package manager
Requires:       linglong-box = %{version}-%{release}
%description    -n linglong-bin
Linglong package management command line tool.

%package        -n linglong-builder
Summary:        Linglong build tools
Requires:       linglong-box = %{version}-%{release} linglong-bin = %{version}-%{release}
%description    -n linglong-builder
This package is a tool that makes it easy to build applications and dependencies.

%package        -n linglong-box
Summary:        Linglong sandbox
%description    -n linglong-box
Linglong sandbox with OCI standard.

%prep
%autosetup -p1 -n linglong-%{version}

%define _debugsource_template %{nil}

%build
export PATH=%{_qt5_bindir}:$PATH
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX:PATH=%{_prefix} \
      -DINCLUDE_INSTALL_DIR:PATH=%{_includedir} \
      -DLIB_INSTALL_DIR:PATH=%{_libdir} \
      -DSYSCONF_INSTALL_DIR:PATH=%{_sysconfdir} \
      -DSHARE_INSTALL_PREFIX:PATH=%{_datadir} \
      -DBUILD_SHARED_LIBS=OFF \
      -DCMAKE_CXX_COMPILER=/usr/bin/g++-12 \
      -DCPM_LOCAL_PACKAGES_ONLY=ON ..
%make_build

%install
cd build
%make_install INSTALL_ROOT=%{buildroot}

%post -n linglong-bin
%systemd_post org.deepin.linglong.PackageManager.service

%preun -n linglong-bin
%systemd_preun org.deepin.linglong.PackageManager.service

%postun -n linglong-bin
%systemd_postun_with_restart org.deepin.linglong.PackageManager.service

%files
%doc README.md
%license LICENSE
%exclude %{_libdir}/cmake/linglong-*/*.cmake

%files -n linglong-bin
%doc README.md
%license LICENSE
%dir %{_sysconfdir}/profile.d
%{_sysconfdir}/profile.d/*
%dir %{_sysconfdir}/X11/Xsession.d
%{_sysconfdir}/X11/Xsession.d/*
%{_bindir}/ll-cli
%{_bindir}/llpkg
%{_bindir}/linglong-repair-tool
%{_bindir}/ll-package-manager
%dir %{_prefix}/lib/%{name}
%dir %{_prefix}/lib/%{name}/container
%{_prefix}/lib/%{name}/container/*
%{_prefix}/lib/sysusers.d/*.conf
%{_prefix}/lib/systemd/system/*.service
%{_prefix}/lib/systemd/system-preset/*.preset
%{_prefix}/lib/systemd/user/*
%dir %{_prefix}/lib/systemd/system-environment-generators
%dir %{_prefix}/lib/systemd/user-environment-generators
%{_prefix}/lib/systemd/system-environment-generators/*
%{_prefix}/lib/systemd/user-environment-generators/*
%dir /usr/libexec
%dir /usr/libexec/linglong
/usr/libexec/%{name}/00-id-mapping
/usr/libexec/%{name}/05-initialize
/usr/libexec/%{name}/20-devices
/usr/libexec/%{name}/30-user-home
/usr/libexec/%{name}/40-host-ipc
/usr/libexec/%{name}/90-legacy
/usr/libexec/%{name}/create-linglong-dirs
/usr/libexec/%{name}/upgrade-all
%dir %{_datadir}/%{name}
%dir %{_datadir}/%{name}/api
%{_datadir}/bash-completion/completions/ll-cli
%{_datadir}/dbus-1/system-services/*.service
%{_datadir}/dbus-1/system.d/*.conf
%{_datadir}/%{name}/config.yaml
%{_datadir}/mime/packages/*
%{_datadir}/%{name}/api/api.json
%{_datadir}/applications/*.desktop

%files -n linglong-builder
%license LICENSE
%{_bindir}/ll-builder
%dir /usr/libexec/%{name}
/usr/libexec/%{name}/fetch-dsc-repo
/usr/libexec/%{name}/fetch-git-repo
/usr/libexec/%{name}/app-conf-generator
%dir /usr/libexec/%{name}/builder
%dir /usr/libexec/%{name}/builder/helper
/usr/libexec/%{name}/builder/helper/*.sh
%dir %{_datadir}/%{name}/builder
%dir %{_datadir}/%{name}/builder/templates
%{_datadir}/bash-completion/completions/ll-builder
%{_datadir}/%{name}/builder/templates/*.yaml

%files -n linglong-box
%license LICENSE
%{_bindir}/ll-box

%changelog
* Thu Apr 25 2024 chenhuixing <chenhuixing@deepin.org> - 1.4.3-1
- Init project
