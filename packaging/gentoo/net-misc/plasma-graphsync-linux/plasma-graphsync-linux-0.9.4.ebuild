# Copyright 2026 Gentoo Authors
# Distributed under the terms of the GNU General Public License v2

EAPI=8

ECM_TEST="forceoptional"
KFMIN=6.20.0
QTMIN=6.9.0
inherit ecm systemd xdg

DESCRIPTION="Sync OneDrive for Business and SharePoint libraries on KDE Plasma"
HOMEPAGE="https://github.com/nvaert1986/plasma-graphsync-linux"
SRC_URI="https://github.com/nvaert1986/${PN}/archive/refs/tags/v${PV}.tar.gz -> ${P}.tar.gz"

LICENSE="GPL-3+ LGPL-3+ CC0-1.0"
SLOT="0"
KEYWORDS="~amd64"

DEPEND="
	>=dev-qt/qtbase-${QTMIN}:6[dbus,gui,network,sql,sqlite,widgets]
	>=dev-qt/qtdeclarative-${QTMIN}:6
	>=dev-qt/qtnetworkauth-${QTMIN}:6
	>=dev-qt/qtwebsockets-${QTMIN}:6
	>=dev-libs/qtkeychain-0.14.2:=[qt6(+)]
	>=kde-frameworks/kbookmarks-${KFMIN}:6
	>=kde-frameworks/kcmutils-${KFMIN}:6
	>=kde-frameworks/kconfig-${KFMIN}:6
	>=kde-frameworks/kcoreaddons-${KFMIN}:6
	>=kde-frameworks/kdbusaddons-${KFMIN}:6
	>=kde-frameworks/kguiaddons-${KFMIN}:6
	>=kde-frameworks/ki18n-${KFMIN}:6
	>=kde-frameworks/kio-${KFMIN}:6
	>=kde-frameworks/knotifications-${KFMIN}:6
	>=kde-frameworks/kwindowsystem-${KFMIN}:6
	kde-plasma/libplasma:6
"
RDEPEND="${DEPEND}
	!net-misc/plasma-onedrive-linux
	>=kde-frameworks/kirigami-${KFMIN}:6
	kde-plasma/plasma-workspace:6
	x11-misc/xdg-utils
"

src_configure() {
	local mycmakeargs=(
		# Ship an organisation's app registration in /etc/xdg/graphsyncrc, e.g. from make.conf:
		# GRAPHSYNC_CLIENT_ID=... GRAPHSYNC_TENANT_ID=...
		-DGRAPHSYNC_CLIENT_ID="${GRAPHSYNC_CLIENT_ID:-}"
		-DGRAPHSYNC_TENANT_ID="${GRAPHSYNC_TENANT_ID:-}"
	)
	ecm_src_configure
}

pkg_postinst() {
	xdg_pkg_postinst
	if [[ ! -e "${EROOT}"/etc/xdg/graphsyncrc ]]; then
		elog "No app registration is installed. Put the Entra ID client and tenant ID in"
		elog "/etc/xdg/graphsyncrc (system-wide) or ~/.config/graphsyncrc:"
		elog "  [Auth]"
		elog "  ClientId=<application id>"
		elog "  TenantId=<directory id>"
	fi
	elog "Each user starts with: plasma-graphsync-linux (setup wizard). The wizard enables"
	elog "the systemd user unit plasma-graphsync-linux.service so syncing starts at login."
}
