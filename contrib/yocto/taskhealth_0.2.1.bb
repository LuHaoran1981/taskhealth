SUMMARY = "Linux user-space thread health monitoring (client-daemon)"
DESCRIPTION = "TaskHealth is a client-daemon thread health monitor for Linux \
user-space, inspired by QNX HAM. The daemon (taskhealthd) accepts registrations \
from multiple processes via Unix domain socket and performs cross-process \
heartbeat monitoring, /proc probes, and alerting. Detects unexpected thread \
exits, futex deadlocks, and lock-wait timeouts."
# TODO: migrate to github.com/symthosm/taskhealth once the org repo exists
HOMEPAGE = "https://github.com/LuHaoran1981/taskhealth"
LICENSE = "MIT & GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://LICENSE.MIT;md5=e60ba40de4a2b0d6e400a41ab5031821 \
                    file://LICENSE;md5=4641e94ec96f98fabc56ff9cc48be14b"
SECTION = "libs"

inherit systemd
SYSTEMD_SERVICE:${PN}-daemon = "taskhealthd.service"

SRC_URI = "https://github.com/LuHaoran1981/taskhealth/releases/download/v${PV}/taskhealth-${PV}.tar.gz"
SRC_URI[sha256sum] = "ba05bc1737a42d9fb21d98de2ee570e753594057fa467726e20f9f0e65398150"

S = "${WORKDIR}/taskhealth-${PV}"

do_compile() {
    oe_runmake all-full
}

do_install() {
    oe_runmake install DESTDIR=${D} prefix=${prefix}
}

# daemon
PACKAGES =+ "${PN}-daemon"
FILES:${PN}-daemon = " \
    ${sbindir}/taskhealthd \
    ${nonarch_libdir}/systemd/system/taskhealthd.service \
    ${mandir}/man8/taskhealthd.8 \
    ${mandir}/man7/taskhealth.7 \
"
RDEPENDS:${PN}-daemon = "${PN}"

# shared library (runtime)
FILES:${PN} = " \
    ${libdir}/libtaskhealth.so.0 \
    ${libdir}/libtaskhealth.so.0.* \
"

# development package (static lib + headers + pkg-config + .so symlink)
FILES:${PN}-dev = " \
    ${libdir}/libtaskhealth.a \
    ${libdir}/libtaskhealth.so \
    ${libdir}/pkgconfig/taskhealth.pc \
    ${includedir}/taskhealth.h \
    ${includedir}/taskhealth_mutex.h \
    ${includedir}/taskhealth/protocol.h \
"

# demo package
PACKAGES =+ "${PN}-demo"
FILES:${PN}-demo = " \
    ${bindir}/taskhealth-demo \
    ${mandir}/man1/taskhealth-demo.1 \
"
RDEPENDS:${PN}-demo = "${PN}"
