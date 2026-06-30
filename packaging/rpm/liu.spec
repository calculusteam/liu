# Liu RPM spec (Fedora COPR source build). OpenGL/X11 on Linux.
# Version is rewritten on release by scripts/update-manifests.sh.
Name:           liu
Version:        0.1.0
Release:        1%{?dist}
Summary:        Native terminal for AI-assisted coding workflows

License:        MIT
URL:            https://liu.software
Source0:        https://github.com/gerizekali/liu/archive/refs/tags/v%{version}.tar.gz#/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  cmake >= 3.20
BuildRequires:  nasm
BuildRequires:  pkgconfig
BuildRequires:  libssh2-devel
BuildRequires:  openssl-devel
BuildRequires:  zlib-devel
BuildRequires:  libX11-devel
BuildRequires:  mesa-libGL-devel
Requires:       libssh2
Recommends:     libnotify
Recommends:     google-noto-sans-mono-fonts

%description
Liu is a native terminal built around AI-assisted ("vibe") coding workflows:
transcript viewing and session resume across installed AI CLIs, AI-driven theme
generation, an in-terminal file browser with a Markdown viewer, split panes, and
prompt translate-on-tab. OpenGL/X11 renderer on Linux. No Electron, no JS VM.

%prep
%autosetup -n liu-%{version}

%build
%cmake -DCMAKE_BUILD_TYPE=Release -DUSE_METAL=OFF
%cmake_build

%install
%cmake_install
# Assets are not a CMake install target — stage under /usr/share/liu (the
# font resolver's FHS fallback), plus the desktop entry + icon.
install -d %{buildroot}%{_datadir}/liu/assets
cp -a assets/. %{buildroot}%{_datadir}/liu/assets/
install -Dm644 packaging/debian/liu.desktop %{buildroot}%{_datadir}/applications/liu.desktop
if [ -f assets/appicon.png ]; then install -Dm644 assets/appicon.png %{buildroot}%{_datadir}/pixmaps/liu.png; fi

%files
%license LICENSE
%{_bindir}/Liu
%{_bindir}/liu-notify
%{_bindir}/liu-notifyd
%{_bindir}/liu-history
%{_bindir}/agenthistory
%{_datadir}/liu/
%{_datadir}/applications/liu.desktop
%{_datadir}/pixmaps/liu.png

%changelog
* Sat Jun 07 2026 calculus.team <hello@liu.software> - 0.1.0-1
- Initial packaged release.
