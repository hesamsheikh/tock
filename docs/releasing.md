# Releasing

A release is what the Mac app offers as an update: the FIRE's firmware and the app, built from
`main`, on a GitHub release. The app only takes proper releases (no drafts or pre-releases) made
from `main`. GitHub Actions builds and publishes them
([`.github/workflows/release.yml`](../.github/workflows/release.yml)).

1. Pick the version and set it in both places:
   - `firmware/tock/system.h`: `#define TOCK_VERSION "0.2.0"`
   - `companion/Info.plist`: `CFBundleShortVersionString`
2. Add a `## 0.2.0 (date)` section to `CHANGELOG.md`: it becomes the release notes, and the
   first lines show in the app.
3. Commit on `main` and push.
4. Tag that commit and push the tag:

       git tag v0.2.0
       git push origin v0.2.0

The tag starts the Release workflow; follow it in the repo's Actions tab. It stops unless the tag
matches both versions, `CHANGELOG.md` has the section, and the tagged commit is on `main`. It
builds the firmware without `secrets.h` (a release is for everyone), builds the app for Apple
silicon and Intel, and publishes the release `v0.2.0` with three files:

| | |
| --- | --- |
| `Tock.zip` | the app |
| `tock-fire.bin` | the firmware |
| `SHA256SUMS` | their SHA-256, which the app checks before it installs anything |

If it stops before publishing, fix the cause on `main`, then move the tag there and push it
again (`git tag -f v0.2.0`, `git push -f origin v0.2.0`). A run that failed on GitHub's side can
be re-run from the Actions tab.

To try the workflow without releasing anything, run it by hand: Actions > Release > Run workflow.
That's a dry run of the branch you pick: the same checks and builds, with the three files attached
to the run instead of published.

The app checks `https://api.github.com/repos/<owner>/<repo>/releases/latest` (`TockReleases` in
`companion/Info.plist`), which needs the repository to be public.

Versions go up only: a FIRE or an app that is already on the release, or newer, is left alone.
