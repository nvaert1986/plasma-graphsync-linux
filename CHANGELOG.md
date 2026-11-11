# Changelog

All notable changes to Graph Sync (plasma-graphsync-linux). Versions follow the last finished
development phase; the third number counts fixes.

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
