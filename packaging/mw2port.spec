Name:           mw2port
Version:        %{_version}
Release:        1%{?dist}
Summary:        Native 64-bit port of MechWarrior 2 (DOS, 1995)
License:        Proprietary
BuildRequires:  gcc, gcc-c++, make, SDL2-devel, mesa-libGL-devel
Requires:       SDL2

%description
Native port of MechWarrior 2 with an OpenGL renderer.
Requires your own copy of the original game data.

%prep
# source is already checked out in the working directory

%build
make -C %{_sourcedir} -j$(nproc) mw2

%install
install -Dm755 %{_sourcedir}/mw2 %{buildroot}%{_bindir}/mw2
mkdir -p %{buildroot}%{_datadir}/mw2port
cp -r %{_sourcedir}/assets %{buildroot}%{_datadir}/mw2port/

%files
%{_bindir}/mw2
%{_datadir}/mw2port
