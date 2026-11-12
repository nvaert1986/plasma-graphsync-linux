# Changelog

All notable changes to Graph Sync (plasma-graphsync-linux). Versions follow the last finished
development phase; the third number counts fixes.

## 0.8.4 (2026-11-12)

**Fixed**
- From a third code review: the settings window, the System Settings page and the sync service
  no longer freeze or hang while a sign-in or token refresh is busy (a sign-out or new settings
  asked for meanwhile are carried out right after it); KWallet is used from one thread only,
  which removes a rare hang when several locations refreshed their sign-in at once; signing in
  restarts only that account's locations, and the tray no longer briefly shows other accounts
  as signed out; a location whose account was removed shows that, instead of using another
  account; `graphsyncctl logout` signs out through the running service; saving the app
  registration signs out once instead of twice; a work account whose tenant is given as a
  domain name takes over the 0.7 locations correctly; ticking "OneDrive of this account" no
  longer changes the current account; the Places entry of a location is found by its exact
  name; the status is read less often, and the settings window no longer waits up to ten
  seconds when another program is setting up the shared accounts.

## 0.8.3 (2026-11-12)

**Fixed**
- No more "Failed to register with host portal" warning when the sync service starts.
- From a second code review: a failed sign-in no longer leaves the new user's tokens with the
  previous account; saving the app registration only affects an account of exactly that
  registration; cancelling Add Account during a sign-in no longer stores the sign-in wrongly;
  the account of 0.7 is moved once even when several programs start together; signing out
  always removes the sign-in from KWallet; Choose Folders uses the location's own account;
  a possible crash of a shortcut location; a Sync button link skips locations whose account is
  gone.

## 0.8.2 (2026-11-11)

**Fixed**
- The Share window named the current account's organisation in "People in …" instead of the one
  of the file's own account.

**Added**
- Man pages for `graphsyncctl`, `graphsyncd` and `plasma-graphsync-linux`; `graphsyncctl --help`
  lists the commands for single files.

**Documentation**
- New screenshots: the name Graph Sync, locations of two accounts, the state of each account in the
  tray.

## 0.8.1 (2026-11-11)

Fixes from testing with two organisations side by side and from a code and security review.

**Fixed**
- A newly added location noticed local changes only at the next regular check (every five
  minutes) until the sync service restarted: its folder is now watched as soon as the first sync
  has made it.
- `graphsyncctl add-root` adds the location with the current account.
- Saving the unchanged app registration while an account of another organisation was current
  could sign that account out; a second user of an organisation could split the first user's
  libraries over two folders; a possible hang of a location while its sign-in was renewed; the
  sign-in of 0.7 is also moved when 0.7 stored it in another spelling.
- The sync service could crash when it was asked to reload the accounts while a reload was still
  reading KWallet (for example right after signing in), and when KWallet reads overlapped. systemd
  restarted it, but syncing paused meanwhile. Present since the first versions.

**Security**
- Every action on a file (Share, links, permissions, Version History, `graphsyncctl sync`) uses
  the account of the location the file is in, and fails if that account is gone, instead of
  falling back to another account.
- A sign-in without a known user is refused; the sign-in from 0.7 can only move to its own user;
  account IDs are checked before they become a KWallet key.
- Locations without an account only go to an account of the same organisation; locations find
  their user again when another app removed the shared account entry.
- Account and organisation names are shown as text, never as markup.
- Apps of the suite that sign the same user in with different app registrations no longer
  overwrite each other's: each keeps its own (`ClientId-<app>` in graphaccountsrc).

## 0.8.0 (2026-11-11)

More than one account.

**Added**
- Add Account (Settings > Accounts, the tray menu, `graphsyncctl add-account`): another work
  account, also of another organisation. With one multi-tenant app registration, each other
  organisation's administrator approves the app once; the app shows the approval page when that
  is still needed. An organisation with its own registration can use its own client ID.
- Settings > Accounts: per account its storage, OneDrive, Sign In or Sign Out, Use for New
  Locations and Remove (stops its locations; the local files stay). Accounts added by another app
  of the suite show under "Other accounts on this computer".
- Settings > Sync lists the locations per account, each with its own Add a Library; a location
  whose account is signed out stays in the list with Sign In.
- The tray shows a signed-out account with its own Sign In, and each account's state.
- `graphsyncctl accounts`, `use-account`, `approval-link`, `add-personal`.

**Changed**
- Accounts are kept in `~/.config/graphaccountsrc`, shared with the other apps of the suite. The
  account from 0.7 moves there by itself, and its sign-in to a KWallet entry of its own, without
  signing in again.
- Share, Copy Link, Version History, View Online and Choose Folders use the account of the
  location the file is in. The SharePoint Sync button uses the account the link was made for, or
  offers to sign it in or add it.
- A pause asked for by Microsoft (throttling) holds back only that account.
- A second user of the same organisation gets the address in the folder names.

**Documentation**
- Credits for KDE and Qt, and a statement that the project is not affiliated with KDE.
- The two Dolphin emblems built on the Breeze emblem icons now carry Breeze's copyright and
  licence (LGPL-3.0-or-later) in REUSE.toml and in the package metadata.

## 0.7.2 (2026-11-11)

Security and reliability fixes from a code and security review.

**Fixed**
- Files online whose names are on the built-in ignore list (temporary, lock and desktop files such
  as `*.tmp`, `*.part`, `*.swp`, `*~`, `.~lock.*`, `.DS_Store`, `Thumbs.db`, `.directory`) and names
  ending in `.cloud` are no longer synced in either direction. Before, such a file was downloaded,
  then taken for a local delete and deleted online (to the recycle bin).
- Saving the same app registration in another spelling (upper case, or a domain instead of the
  tenant ID) no longer signs you out: the sign-in moves along. IDs are stored in one spelling.
- Taking over plasma-onedrive-linux: it runs in the background with a page that says so (and Try
  Again), from the app, System Settings and every `graphsyncctl` command. Settings changed meanwhile
  no longer cancel it; a marker records when it is done. A busy old service is retried later
  instead of copied mid-write; a permanent error no longer makes the sync service restart in a loop.
  The old service is masked before it is stopped. Default applications are updated atomically, also
  in `kde-mimeapps.list`.
- "Show the Tray Icon" is only offered when a system tray exists.
- A new file online at the path of an earlier opened file gets its folder's Files On-Demand choice.

**Security**
- SharePoint Sync button links (`odopen://`) are checked: SharePoint host, GUIDs; site and library
  names come from Microsoft instead of the link and are shown as plain text.
- A sync location can no longer be `/`, the home folder, a folder above it, or overlap another
  location; a folder that already holds files gets a warning (its files would be uploaded).
- The sync state (file names, upload addresses) is readable by the owner only; copies of old
  versions are owner-only and removed after a day and at sign-out.
- The sign-in token is only sent over https to Microsoft Graph; redirects to plain http are not
  followed; error messages no longer contain download addresses.
- Names and IDs from the server are checked before they are used in local paths.
- Links to local files with `#` or `%` in their name open the right file.
- The tray's "launch" request only passes the app's normal options.

## 0.7.1 (2026-11-11)

**Fixed** (from a code review)
- Taking over plasma-onedrive-linux also works when System Settings is opened first, never copies
  while the old service still runs, and reads an administrator's app registration from every system
  configuration folder.
- "Show the Tray Icon" restarts the panel the right way whether or not Plasma runs under systemd.
- One definition of the sign-in's KWallet entry.

## 0.7.0 (2026-11-11)

First public version, under the new name.

**Changed**
- New name plasma-graphsync-linux, shown as **Graph Sync**: a sync client for Microsoft OneDrive and
  SharePoint on KDE Plasma. New IDs (`io.github.nvaert1986.graphsync`), programs `graphsyncd` and
  `graphsyncctl`, settings in `~/.config/graphsyncrc`.

**Added**
- An install of plasma-onedrive-linux (the name before 0.7.0) is taken over once: settings, sync
  state, sign-in and default applications; nothing is downloaded again. The packages replace it.
- CONTRIBUTING.md, SECURITY.md (private vulnerability reports), issue templates, REUSE licensing.

**Fixed**
- A file opened from a folder set to "Free Up Space" is no longer freed again at the next sync
  (it becomes "available on this device", as on Windows).

## Earlier versions (as plasma-onedrive-linux)

Developed and tested privately before the first public release:

- **0.6.x**: Files On-Demand with placeholders, offline handling, resumable uploads, throttling,
  packages for Debian 13, Fedora and Arch, the app registration in the setup wizard.
- **0.5.x**: "Add shortcut to My files" folders, bandwidth limits, exclusions and `.nosync`,
  low disk space protection, sign-in with a code.
- **0.4.x**: Share, Copy Link, Version History and View Online.
- **0.2 – 0.3**: setup wizard, tray applet, System Settings page, Dolphin integration, SharePoint
  Sync button.
- **0.1**: the two-way sync engine on Microsoft Graph.
