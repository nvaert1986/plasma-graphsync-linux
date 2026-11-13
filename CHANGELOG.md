# Changelog

All notable changes to Graph Sync (plasma-graphsync-linux). Versions follow the last finished
development phase; the third number counts fixes.

## 0.9.4 (2026-11-13)

From the remaining findings of the review of the sync service, and from a sync cycle that hung while
Microsoft's push notification service was down.

**Fixed**
- A shortcut ("Add shortcut to My files") that was removed online, moved out of the folder or no
  longer synced moved its folder to the Trash at once, with changes made here that were not
  uploaded yet. Those are now uploaded to the library first.
- While Microsoft's push notification service was unavailable, every sync cycle waited up to many
  minutes for it, and its refusals held back the account's other requests. It is now asked once,
  and again after half an hour; polling covers the time in between.
- Pausing, stopping a location and closing the sync service no longer wait for a request that is
  still running (up to two minutes); several locations also stop at the same time instead of one
  after the other.
- Pausing for a very long time (about 25 days or more) ended the pause at once.
- A request to keep files on this device with an unknown choice is refused instead of
  downloading everything selected.
- Dolphin: names with spaces at the start or end got no emblem; emblems could stay missing after
  the sync service restarted; the emblem cache no longer grows without limit.
- Download Now on many online-only files downloads them side by side instead of one after the
  other.
- View Online without a connection opened the wrong address for a file in a shortcut folder.
- Making another account current or signing one out while a sign-in was still running could
  store that sign-in under the wrong account; it now waits until the sign-in is done.
- Starting syncing another account's OneDrive could name its folder after the current account's
  organization.
- `plasma-graphsync-linux --share` and `--versions` with a relative path, while the window was
  already open, used the folder of the first start.
- Opening an online-only or `.url` file, Download Now, Copy Link and View Online no longer start
  the settings window's background work first (taking over an old install, organization lookup).
- Sharing and Version History show every sharing link and version, not only the first page.
- Taking over plasma-onedrive-linux settings could lose the settings from before the takeover
  when copying failed (disk full) and was tried again.

## 0.9.3 (2026-11-12)

From new automated tests with large files, Office files, two accounts and network loss, and from a
full code and security review of the sync service and everything that takes input from outside
(the security review found no vulnerabilities).

**Fixed**
- A location stopped because its folder was removed or moved (an unplugged disk) could start
  syncing again after a pause, a resume or a change of settings.
- "Delete Online Too" in an older "many files removed" notification no longer applies to items
  that went missing after it; the question is asked again.
- Sharing, changing access to or restoring a version of a file that is new or renamed here and not
  online yet no longer acts on the old online file of that name; the app also keeps the items it
  looked up only briefly.
- KWallet not open yet at login no longer counts as signed out, and no longer switches the current
  account; it is tried again for a few minutes.
- Opening an online-only file works while syncing is paused, and one that was waiting while its
  location restarted no longer waits forever.
- Removing a location also removes its database's log files (a shortcut added again could get
  old state back).
- `graphsyncctl`: add-root checks the folder like the app does and goes through the running sync
  service; sync refuses a location the sync service already syncs; remove-root and sync take a
  name only when one location has it; download uses the file's own account.
- File, folder and account names in notifications are shown as text, never as links or markup.
- PC folder backup no longer starts when the state of the OneDrive location cannot be read; Stop
  Backup says that the OneDrive location keeps leaving the folder out.
- A large upload (over 4 MB) cut off by a crash or power loss was not resumed: the new upload
  clashed with the unfinished one and the file ended up online as a conflict copy of itself. The
  upload is now resumed, and an unfinished leftover online is replaced.

## 0.9.2 (2026-11-12)

Fixes from a full review of the sync engine, with new automated tests against a real OneDrive.
Most of these concern versions before 0.9 as well.

**Fixed: files lost or deleted by mistake** (to the recycle bin online, unless noted)
- An online-only file moved into a new folder here was deleted online.
- A file or folder renamed here to a name SharePoint does not allow (such as one with ":") was
  deleted online, for a folder with everything in it. It now stays as it is online, with a message.
- A move or rename here that failed online for another reason (a lock, a server error) deleted the
  item online; it now stays and is tried again.
- A file deleted online and uploaded again under the same name (within one sync) was deleted
  here and online.
- Changes made here were lost (also not in the recycle bin) when a file changed here and online
  had a name too long for a conflict copy, or when a download finished over a new file saved
  where an online-only file was, or over a change made while it downloaded. Such work is now kept
  as a conflict copy, or the download waits.
- A folder renamed online to a name too long for this computer was thrown away here at the
  regular full check.
- Two files whose names were swapped online ended up with conflict names online.
- With a Files On-Demand folder location, a new online folder could download its files twice at
  the same time; the hash check caught it, but without one a damaged file could have been kept.
- A folder deleted here was deleted online without checking for new files in it when that check
  failed; a file briefly missing (an editor saving it) could be deleted online; pausing during a
  sync could remove files here that were just being moved online; a small location (50 items or
  fewer) whose folder was emptied, such as an unmounted disk, was not protected by the
  "many files removed" question.
- Freeing up space could remove a file that was saved at that very moment.

**Fixed: other**
- One file that cannot be read here (permissions) no longer stops all other uploads with a false
  "no connection" state.
- Renaming a folder with many files here no longer asks the "many files removed" question.
- With too little disk space for downloads, changes made here are still uploaded.
- An item that failed and could not be fetched again because of a server error is no longer
  forgotten until the next full check; "Check All Files Again" and a changed folder choice made
  during a sync are no longer undone by that sync; a file opened while a sync downloads is fetched
  between two batches instead of in the middle of one.

## 0.9.1 (2026-11-12)

Fixes from a code and security review of 0.9.0 (the security review found no vulnerabilities).

**Fixed**
- PC folder backup: when adding the backup location fails, the OneDrive location no longer keeps
  leaving that folder out (it would have been synced nowhere); a backup is no longer refused for
  good when the OneDrive location shows an error for an unrelated file; trying again after one
  folder failed only starts the folders not backed up yet.
- The "Shared" emblem no longer disappears from a file that is downloaded again after a failed
  attempt; removing the last link of a OneDrive file takes the emblem away (the owner's own
  permission alone is no sharing).
- Automatic free up space leaves files alone that are about to be downloaded because they changed
  online, and recognises Word's lock file for every name length.

**Changed**
- The sync service only watches which files programs open while automatic free up space is on for
  that location, and only keeps that record then; builds and checkouts in synced folders no longer
  wake it otherwise.
- Previews of online-only files are fetched by the file's id instead of its path; previews that
  Dolphin stopped waiting for are no longer fetched, and several requests for the same file are
  fetched once.
- A test setting for automatic free up space is only built into development builds, never into
  packages.

## 0.9.0 (2026-11-12)

Phase 9: closer to the Windows client.

**Added**
- PC folder backup (Settings > Sync > Back Up Folders…, or `graphsyncctl backup-folders`):
  Desktop, Documents and Pictures kept in the account's OneDrive, in the folders of the same names
  the Windows client uses. The folders stay where they are on this computer, keep their icons and
  each becomes a location of its own; the OneDrive location leaves those folders out. Files
  already in OneDrive there come down; a file with the same name but other content is kept as a
  copy. A backup starts only when the OneDrive location is up to date, so no changes stay behind
  in the folder it leaves out. Stop Backup ends the location and keeps the files here and online.
- A "Shared" emblem in Dolphin on files and folders shared with others, and on what is inside a
  shared folder: the same items the OneDrive website marks as shared. A share, invitation or
  removed link from this computer (Share window, Copy Link, `graphsyncctl`) shows within
  seconds; sharing changed elsewhere shows with the item's next change or the regular full
  check, because Microsoft Graph does not report sharing changes to sync clients. After the last
  link of a SharePoint file is removed the file keeps its own permissions, so it stays marked,
  like on the website. Each location reads its full listing once after the update.
- Previews of online-only files in Dolphin and the file dialogs, made by Microsoft and fetched
  without downloading the files (a thumbnail plugin for the placeholders). Dolphin with its own
  list of preview plugins: switch it on in Settings > Advanced > Files On-Demand.
- Automatic free up space (Settings > Advanced > Files On-Demand): files not opened for a chosen
  number of days become online-only again, in locations with Files On-Demand on. Files kept on
  this computer, files with changes not uploaded yet and files a program has open (or an office
  lock file next to them) stay. Works on disks mounted with noatime too: the sync service notes
  which files are opened. Off by default.

**Fixed**
- The tray widget showed "Nothing is synced yet" and no status on Plasma 6.3 (Debian 13); it now
  works with every Plasma 6 version.

**Changed**
- Free Up Space leaves a file alone while a program has it open; it is freed once it is closed.
- `graphsyncctl remove-root` stops the location in the running sync service at once.
- The sync service no longer reads every small file in each cycle to look for renamed
  placeholders, only new and changed ones.

## 0.8.5 (2026-11-12)

**Fixed**
- Files On-Demand: a file that became online-only (Free Up Space, or a new file from online
  while Files On-Demand is on) was deleted online right away (to the recycle bin), because the
  check for local deletes no longer saw its placeholder. Present since 0.7.1; Files On-Demand is
  off by default. Files deleted this way can be restored from the site's or OneDrive's recycle
  bin.

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
