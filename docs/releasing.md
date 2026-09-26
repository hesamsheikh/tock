# Releasing

A release is what the Mac app offers as an update: the FIRE's firmware and the app, built from
`main`, on a GitHub release. The app only takes proper releases (no drafts or pre-releases) made
from `main`.

1. Pick the version and set it in both places:
   - `firmware/tock/system.h`: `#define TOCK_VERSION "0.2.0"`
   - `companion/Info.plist`: `CFBundleShortVersionString`
2. Add a `## 0.2.0 (date)` section to `CHANGELOG.md`: it becomes the release notes, and the
   first lines show in the app.
3. Commit on `main` and push.
4. Run `./release.sh`.

`release.sh` stops unless you're on `main`, with nothing uncommitted, level with `origin/main`,
and the version isn't released yet. It builds the firmware without `secrets.h` (a release is for
everyone), builds the app for Apple silicon and Intel, and publishes the release `v0.2.0` with
three files:

| | |
| --- | --- |
| `Tock.zip` | the app |
| `tock-fire.bin` | the firmware |
| `SHA256SUMS` | their SHA-256, which the app checks before it installs anything |

It needs the [GitHub CLI](https://cli.github.com), signed in. The app checks
`https://api.github.com/repos/<owner>/<repo>/releases/latest` (`TockReleases` in
`companion/Info.plist`), which needs the repository to be public.

Versions go up only: a FIRE or an app that is already on the release, or newer, is left alone.
