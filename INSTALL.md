# Installing plasma-graphsync-linux

## Installing a package

Ready-made packages exist for Debian 13, Fedora and Arch Linux. As root:

```
apt install ./plasma-graphsync-linux_<version>-1_amd64.deb          # Debian 13
dnf install ./plasma-graphsync-linux-<version>-1.fc45.x86_64.rpm     # Fedora
pacman -U plasma-graphsync-linux-<version>-1-x86_64.pkg.tar.zst      # Arch Linux
```

The package manager installs the dependencies. Then continue with section 5 (first start); the
setup wizard asks for the app registration's Client ID and your work e-mail address. The rest of
this guide builds the client from source; the build files for these packages and for Gentoo are
in [packaging/](packaging/).

## 1. Requirements

- KDE Plasma 6 with Dolphin, on Wayland or X11
- Qt 6.9 or newer and KDE Frameworks 6.20 or newer (recommended)
- Also works with Qt 6.8 and KDE Frameworks 6.13, for example on Debian 13 (trixie). There the
  browser sign-in is not available (it needs Qt 6.9): every **Sign In** button signs in with a
  code instead. You open the shown address on any device and enter the code. Everything else
  is the same
- A secret store for the sign-in token: KWallet (or another Secret Service provider)
- An Entra ID app registration in your Microsoft 365 tenant (section 2)

Build dependencies per distribution:

**Gentoo**

```
emerge --ask --noreplace dev-build/cmake dev-build/ninja kde-frameworks/extra-cmake-modules \
    dev-qt/qtbase dev-qt/qtdeclarative dev-qt/qtnetworkauth dev-qt/qtwebsockets dev-libs/qtkeychain \
    kde-frameworks/kbookmarks kde-frameworks/kcmutils kde-frameworks/kconfig kde-frameworks/kcoreaddons \
    kde-frameworks/kdbusaddons kde-frameworks/kguiaddons kde-frameworks/ki18n kde-frameworks/kio \
    kde-frameworks/kirigami kde-frameworks/knotifications kde-frameworks/kwindowsystem \
    kde-plasma/libplasma sys-devel/gettext
```

`dev-qt/qtbase` needs the USE flags `dbus gui network sql sqlite widgets` (the defaults on a
Plasma system), `dev-libs/qtkeychain` needs `qt6`.

**Arch Linux**

```
pacman -S --needed base-devel cmake ninja extra-cmake-modules qt6-base qt6-declarative \
    qt6-networkauth qt6-websockets qtkeychain-qt6 kbookmarks kcmutils kconfig kcoreaddons \
    kdbusaddons kguiaddons ki18n kio kirigami knotifications kwindowsystem libplasma
```

**Debian testing / unstable**

```
apt install cmake ninja-build extra-cmake-modules gettext qt6-base-dev qt6-declarative-dev \
    qt6-networkauth-dev qt6-websockets-dev qtkeychain-qt6-dev libkf6bookmarks-dev libkf6config-dev \
    libkf6coreaddons-dev libkf6dbusaddons-dev libkf6guiaddons-dev libkf6i18n-dev libkf6kcmutils-dev \
    libkf6kio-dev libkf6notifications-dev libkf6windowsystem-dev libplasma-dev \
    libqt6sql6-sqlite qml6-module-org-kde-kirigami qml6-module-qtquick-dialogs
```

The same command works on Debian 13 (trixie); see Requirements for its code-only sign-in.

## 2. App registration (once per organisation)

The client signs in with its own app registration, not Microsoft's. An administrator creates it
once in the Entra admin center (Identity > Applications > App registrations > New registration):

| Setting | Value |
|---|---|
| Supported account types | Accounts in this organizational directory only (single tenant) |
| Platform | Mobile and desktop applications, redirect URI `http://localhost` (no Web or SPA platform) |
| Allow public client flows | Yes |
| Client secret / certificate | None (public client with PKCE) |
| API permissions (delegated, Microsoft Graph) | `Files.ReadWrite.All`, `Sites.ReadWrite.All`, `User.Read`, `offline_access`, `openid`, `profile` |
| Admin consent | Grant once for the organisation |

Optional: `People.Read` and `User.ReadBasic.All` (delegated, with admin consent) give name
suggestions when inviting people in the Share window. Without them everything else works and
people are invited by typing their e-mail address; the client falls back by itself. Granted later,
suggestions appear after the next token renewal (within an hour) or at once after signing out and
in. Check from a terminal with `graphsyncctl people <part of a name>`.

Recommended: in Enterprise applications, set **Assignment required** to Yes and assign only the
Linux users. Note the **Application (client) ID** and **Directory (tenant) ID**.

## 3. Build

```
git clone https://github.com/nvaert1986/plasma-graphsync-linux.git
cd plasma-graphsync-linux
cmake -S . -B build -G Ninja -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

To ship the app registration for all users of the machine, add
`-DGRAPHSYNC_CLIENT_ID=<client id> -DGRAPHSYNC_TENANT_ID=<tenant id>` to the first command; it is then
installed as `/etc/xdg/graphsyncrc`.

Optional: run the unit tests with `ctest --test-dir build`.

## 4. Install

As root:

```
cmake --install build
```

This installs:

- `plasma-graphsync-linux` (setup wizard, Settings, Share and Version history windows) and its
  `odopen://` handler for the SharePoint **Sync** button
- `graphsyncd`, the sync service, as the systemd user unit `plasma-graphsync-linux.service`
- `graphsyncctl`, a command line tool for status, support and tests
- the system tray applet, the page in System Settings > Network, notifications
- the Dolphin plugins (status emblems, right-click menu) and icons
- the Dutch translation

If you did not pass the IDs at build time, there are three ways to set them:

- **In the setup wizard** (easiest for users): on first start it asks for the **Client ID** and
  the user's **work e-mail address**; the tenant ID is looked up from the e-mail domain. The
  Client ID field also takes a pasted setup text, so IT can hand out one line:

  ```
  ClientId=<application (client) id> TenantId=<directory (tenant) id>
  ```

  The IDs can be changed later in Settings > Advanced > App Registration (this signs out).
- **For all users of a machine**, in `/etc/xdg/graphsyncrc` (the wizard then skips the question), or
  for one user in `~/.config/graphsyncrc`:

  ```
  [Auth]
  ClientId=<application (client) id>
  TenantId=<directory (tenant) id>
  ```
- **From a terminal**: `graphsyncctl configure --client-id <id> --tenant-id <tenant id, domain or e-mail address>`

## 5. First start (per user)

1. Run `plasma-graphsync-linux` from the application menu or a terminal. The setup wizard signs
   you in through the browser, asks for a base folder (default: your home folder) and sets up
   `OneDrive - <Organisation>`. It also enables and starts the sync service.
2. Add SharePoint libraries with **Add a Library…** in the tray applet, or with the **Sync**
   button on a SharePoint library in the browser. They go to `SharePoint - <Organisation>/<Site> - <Library>`.
3. Restart the panel once so the tray applet appears, or log out and back in:
   ```
   systemctl --user restart plasma-plasmashell.service
   ```
4. Close and reopen Dolphin to load the status emblems and the right-click menu.

If another program claimed the SharePoint Sync button before, make this client the handler:

```
xdg-mime default io.github.nvaert1986.graphsync.desktop x-scheme-handler/odopen
```

"Add shortcut to My files" on a single file puts a Windows internet shortcut (`name.url`) in
your OneDrive. The client opens those in the browser (http and https addresses only). Wine
registers itself for this file type too; make the client the default:

```
xdg-mime default io.github.nvaert1986.graphsync.desktop application/x-mswinurl
```

## Updating

Pull, build and install again, then restart the service, the panel and Dolphin:

```
git pull
cmake --build build
cmake --install build          # as root
systemctl --user restart plasma-graphsync-linux.service plasma-plasmashell.service
```

## Coming from plasma-onedrive-linux

Before 0.7.0 this client was called plasma-onedrive-linux. The new package replaces the old one,
and the existing setup is taken over once, at the next login or when you first open the app:
settings, sync state (nothing is downloaded again), the sign-in and the default handlers move
over, and the old sync service is stopped and disabled. Log out and back in after installing.

## Installing for one user only (testing)

Without root, install into `~/.local`:

```
cmake -S . -B build-user -G Ninja -DCMAKE_INSTALL_PREFIX=$HOME/.local \
    -DKDE_INSTALL_SYSTEMDUSERUNITDIR=$HOME/.local/share/systemd/user
cmake --build build-user
cmake --install build-user
```

Plasma does not look for plugins in `~/.local` by default, so System Settings and Dolphin would
miss the page and the plugins. Add the plugin folder to the session (the folder is
`~/.local/lib64/plugins` on Gentoo, `~/.local/lib/plugins` on Arch, and
`~/.local/lib/x86_64-linux-gnu/plugins` on Debian; `build-user/install_manifest.txt` shows it):

```
mkdir -p ~/.config/plasma-workspace/env
echo 'export QT_PLUGIN_PATH="$HOME/.local/lib64/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"' \
    > ~/.config/plasma-workspace/env/plasma-graphsync-linux.sh
```

Then log out and back in, and continue with section 5. `~/.local/bin` must be in `PATH`.

The install registers the file type of Files On-Demand placeholders (`*.cloud`) in
`~/.local/share/mime` and refreshes that database; if `update-mime-database` was not found,
run `update-mime-database ~/.local/share/mime` once.

## Uninstall

Per user, first stop the service:

```
systemctl --user disable --now plasma-graphsync-linux.service
```

Then remove the files (as root for a system install):

```
xargs rm -f < build/install_manifest.txt
```

Your synced folders stay as ordinary files. Also remove, if you like:

- `~/.config/graphsyncrc` (settings) and `~/.local/state/graphsync` (sync state)
- the sign-in token: the "plasma-graphsync-linux" entry in KWallet
- the Places entries "OneDrive - <Organisation>" and "SharePoint - <Organisation>" in Dolphin
- for a user install: `~/.config/plasma-workspace/env/plasma-graphsync-linux.sh`
