# Packaging

| Distribution | Files | Build |
|---|---|---|
| Gentoo | `gentoo/net-misc/plasma-graphsync-linux/` | copy into an overlay, `ebuild ... manifest`, `emerge net-misc/plasma-graphsync-linux` |
| Arch | `arch/PKGBUILD` | `makepkg -si` |
| Debian | `debian/` | copy to `debian/` in the source tree, `dpkg-buildpackage -us -uc -b` |
| Fedora | `rpm/plasma-graphsync-linux.spec` | `rpmbuild -ba --define "_sourcedir $PWD" plasma-graphsync-linux.spec` with the source tarball next to it |

All of them leave the app registration out by default: users enter the Client ID and their
work e-mail address in the setup wizard. To ship an organisation's registration in
`/etc/xdg/graphsyncrc` instead, set `GRAPHSYNC_CLIENT_ID` and `GRAPHSYNC_TENANT_ID` in the environment (Arch,
Debian, Gentoo) or pass `--define "graphsync_client_id <id>" --define "graphsync_tenant_id <id>"` to
rpmbuild (Fedora).

Requirements: Qt 6.9 and KDE Frameworks 6.20 or newer are the main target. Qt 6.8 with KDE
Frameworks 6.13 (Debian 13 trixie, Plasma 6.3) builds and works too, but signs in with a code only,
because the browser sign-in needs Qt 6.9. The Debian package therefore builds on trixie as well
as on testing/unstable.

## Tested

0.6.5, built and install-tested in clean containers (podman): Debian 13 (Qt 6.8, code-only
sign-in), Fedora 45 and Arch Linux. Each package installed with its dependencies, the setup
wizard started without missing QML modules, `graphsyncctl` ran, the systemd user unit was in place,
and removal left nothing behind. lintian, rpmlint and namcap show no errors; the remaining
warnings are missing man pages, the System Settings page's `systemsettings` command and (Arch)
`xdg-utils`, which is started at run time. A container does not run a Plasma session: the tray
applet, Dolphin plugins and System Settings page are tested on real desktops.
