Name:           oxenmq
Version:        1.2.7
Release:        1%{?dist}
Summary:        zeromq-based Oxen message passing library

License:        BSD
URL:            https://github.com/oxen-io/oxen-mq
Source0:        %{name}-%{version}.src.tar.gz

%global sonamever %{version}

BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig
BuildRequires:  cppzmq-devel

Patch1: fully-version-library.patch

%description
This C++17 library contains an abstraction layer around ZeroMQ to support
integration with Oxen authentication, RPC, and message passing.  It is designed
to be usable as the underlying communication mechanism of SN-to-SN communication
("quorumnet"), the RPC interface used by wallets and local daemon commands,
communication channels between oxend and auxiliary services (storage server,
lokinet), and also provides a local multithreaded job scheduling within a
process.

%package libs
Summary: zeromq-based Oxen message passing library

%description libs
This C++17 library contains an abstraction layer around ZeroMQ to support
integration with Oxen authentication, RPC, and message passing.  It is designed
to be usable as the underlying communication mechanism of SN-to-SN communication
("quorumnet"), the RPC interface used by wallets and local daemon commands,
communication channels between oxend and auxiliary services (storage server,
lokinet), and also provides a local multithreaded job scheduling within a
process.

This package contains the library file needed by software built using oxenmq.

%package devel
Summary: zeromq-based Oxen message passing library -- headers
Requires: pkgconfig
Requires: cppzmq-devel
Requires: %{name}-libs%{?_isa} = %{version}-%{release}

%description devel
This C++17 library contains an abstraction layer around ZeroMQ to support
integration with Oxen authentication, RPC, and message passing.  It is designed
to be usable as the underlying communication mechanism of SN-to-SN communication
("quorumnet"), the RPC interface used by wallets and local daemon commands,
communication channels between oxend and auxiliary services (storage server,
lokinet), and also provides a local multithreaded job scheduling within a
process.

This package contains the library headers and other development files needed to
build software using oxenmq.

%prep

%autosetup

%build

%undefine __cmake_in_source_build
%cmake -DOXENMQ_INSTALL_CPPZMQ=OFF -DOXENMQ_LOKIMQ_COMPAT=OFF
%cmake_build

%install

%cmake_install


%files devel

%{_includedir}/oxenmq/*.h
%{_libdir}/liboxenmq.so
%{_libdir}/pkgconfig/liboxenmq.pc

%files libs

%license LICENSE
%doc README.md
%{_libdir}/liboxenmq.so.%{version}



%changelog
* Fri Oct 15 2021 Technical Tumbleweed <necro_nemesis@hotmail.com> -1.2.7~1
- Merge dev changes 1.2.7
- change branch Catch2 to 2.x
- bump version

* Mon Aug 09 2021 Jason Rhinelander <jason@imaginary.ca> - 1.2.6-2
- Split oxenmq into lib and devel package
- Versioned the library, as we do for debs
- Updated various package descriptions and build commands

* Thu Jul 22 2021 Technical Tumbleweed <necro_nemesis@hotmail.com> oxenmq
-First oxenmq RPM
-Second build update to v1.2.6
