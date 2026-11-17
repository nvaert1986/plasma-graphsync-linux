# Contributing

Thanks for helping with plasma-graphsync-linux. Bug reports, tests on other distributions,
translations and code are all welcome.

## Reporting bugs and ideas

Use the issue templates on GitHub. For anything that touches files (sync, deletes, conflicts),
include the steps that lead to it and the log (see the bug template). **Remove names, e-mail
addresses, organisation and site names from logs before posting them.** Security problems go
through the security policy in [SECURITY.md](SECURITY.md), not public issues.

## Building and testing

See [INSTALL.md](INSTALL.md) for the dependencies. For development:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build
```

A user install for trying changes on your own desktop is described in INSTALL.md ("Installing
for one user only"). Test sync changes on a test library or test site, never on data you cannot
lose, and keep the online recycle bin in mind.

## Versions and branches

Graph Sync, Graph Mail and their shared core (plasma-graphcore-linux) keep one version series.

- The suite shares **major.minor**; the patch number may differ per app (one app at 0.11.1 next to
  another at 0.11.0).
- Within a release series (0.11.x) everything the apps share stays compatible: the shared core's
  library interface (ABI and API), the D-Bus interfaces, and the configuration, cache and state
  formats (new settings keys are fine; older versions ignore them). A patch release holds fixes,
  and small features only when they change none of these. Anything else waits for the next minor
  (0.12.0) or major (1.0.0).
- Each series has a maintenance branch named after it (`0.11`). Fixes go there and are merged into
  `main`; `main` holds the next series.

## Pull requests

- Keep a pull request to one topic, and describe what changed and how you tested it.
- The build must stay free of warnings and `ctest` must pass. Add a unit test in `autotests/`
  where the logic allows it.
- Follow the style of the surrounding code: Qt and KDE Frameworks conventions, comments that say
  why rather than what, user-facing texts through `i18n()`.
- New files get an SPDX header (`SPDX-FileCopyrightText: <year> plasma-graphsync-linux
  contributors` and `SPDX-License-Identifier: GPL-3.0-or-later`), or an entry in `REUSE.toml` when
  the format has no comments.
- By contributing you agree that your work is licensed under GPL-3.0-or-later.

## Translations

Translations live in `po/<language>/`. Extract fresh templates with `Messages.sh` (the KDE
convention: run it with `XGETTEXT` and `podir` set, as KDE's tooling does), then update the `.po`
files with your translation tool. English is the source language; Dutch is included.

## How this project is made

The code, tests and documentation were written by an AI model (Claude, by Anthropic), directed,
reviewed and tested by the maintainer (see the README). Human contributions are reviewed and
tested the same way before they are merged.
