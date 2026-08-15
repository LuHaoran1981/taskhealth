SUMMARY = "Linux user-space thread health monitoring (client-daemon)"
DESCRIPTION = "TaskHealth is a client-daemon thread health monitor for Linux \
user-space, inspired by QNX HAM. The daemon (taskhealthd) accepts registrations \
from multiple processes via Unix domain socket and performs cross-process \
heartbeat monitoring, /proc probes, and alerting. Detects unexpected thread \
exits, futex deadlocks, and lock-wait timeouts."
HOMEPAGE = "https://github.com/symthosm/taskhealth"
LICENSE = "MIT & GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://LICENSE.MIT;md5=e60ba40de4a2b0d6e400a41ab5031821 \
                    file://LICENSE;md5=4641e94ec96f98fabc56ff9cc48be14b"
SECTION = "libs"

SRC_URI = "https://github.com/symthosm/taskhealth/releases/download/v${PV}/taskhealth-${PV}.tar.gz"
SRC_URI[sha256sum] = "<fill-in-on-release>"

S = "${WORKDIR}/taskhealth-${PV}"

do_compile() {
    oe_runmake all-full
}

do_install() {
    oe_runmake install DESTDIR=${D} prefix=${prefix}
}

# daemon
PACKAGES =+ "${PN}-daemon"
FILES:${PN}-daemon = "${sbindir}/taskhealthd"
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
FILES:${PN}-demo = "${bindir}/taskhealth-demo"
RDEPENDS:${PN}-demo = "${PN}"
