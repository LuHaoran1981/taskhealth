Name:           taskhealth
Version:        0.1.0
Release:        1%{?dist}
Summary:        Linux thread health monitoring (client-daemon)

License:        MIT AND GPL-2.0-or-later
# TODO: migrate to github.com/symthosm/taskhealth once the org repo exists
URL:            https://github.com/LuHaoran1981/taskhealth
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc, make

%description
TaskHealth is a client-daemon thread health monitor for Linux user-space,
inspired by QNX HAM. The daemon (taskhealthd) accepts registrations from
multiple processes via Unix domain socket and performs cross-process
heartbeat monitoring, /proc probes, and alerting.

Detects unexpected thread exits, futex deadlocks, and lock-wait timeouts.

%package -n taskhealthd
Summary:  TaskHealth monitoring daemon

%description -n taskhealthd
The taskhealthd daemon — Unix domain socket server that monitors registered
threads across processes. Runs the watchdog detection engine and outputs
alerts to stderr, syslog, a log file, or an external script.

%package -n libtaskhealth0
Summary:  TaskHealth client shared library

%description -n libtaskhealth0
Client library linked into business processes. Communicates with taskhealthd
via Unix domain socket (SOCK_SEQPACKET) to register threads, send heartbeats,
and receive responses.

%package devel
Summary:  Development files for %{name}
Requires: libtaskhealth0 = %{version}-%{release}

%description devel
Headers, static library, shared library symlink, and pkg-config file
for developing applications that use TaskHealth.

%package demo
Summary:  Demo program for %{name}
Requires: libtaskhealth0 = %{version}-%{release}

%description demo
TaskHealth demo demonstrating auto-name, unexpected exit, deadlock,
normal heartbeat, and lock-wait timeout scenarios.

%prep
%setup -q

%build
make all-full

%install
make install DESTDIR=%{buildroot} prefix=/usr

%files -n taskhealthd
%license LICENSE
%{_sbindir}/taskhealthd

%files -n libtaskhealth0
%license LICENSE.MIT
%{_libdir}/libtaskhealth.so.0
%{_libdir}/libtaskhealth.so.0.1.0

%files devel
%license LICENSE.MIT
%{_libdir}/libtaskhealth.a
%{_libdir}/libtaskhealth.so
%{_libdir}/pkgconfig/taskhealth.pc
%{_includedir}/taskhealth.h
%{_includedir}/taskhealth_mutex.h
%{_includedir}/taskhealth/protocol.h

%files demo
%{_bindir}/taskhealth-demo

%changelog
* Tue Aug 12 2026 Lu Haoran <37183985@qq.com> - 0.1.0-1
- Initial release with client-daemon architecture
