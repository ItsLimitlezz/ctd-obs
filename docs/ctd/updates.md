# CTD OBS updates

CTD OBS checks GitHub Releases of this repository for a newer CTD OBS and offers to install it. OBS Studio's own
updaters stay off: the Windows updater (which points at obsproject.com) is disabled in `frontend/obs-main.cpp`, and the
macOS build has no Sparkle (`ctd-macos.yaml` fails if it is bundled).

Code: `frontend/ctd/ctd-update.cpp` (Qt: launch check, dialog, download, install), `frontend/ctd/ctd-splash.cpp` (splash
screen), `frontend/ctd/ctd-update-logic.cpp` (version compare, release parsing, the time-limited check; unit tests in
`test/ctd-update`), hooked in `frontend/obs-main.cpp` and `frontend/widgets/OBSBasic.cpp`. CI:
`.github/workflows/ctd-build.yaml`.

## Channels

| Channel | Offers | Tags |
|---|---|---|
| **Stable** (default) | published releases only | `ctd-v<X.Y.Z>`, e.g. `ctd-v0.10.0` |
| **Test builds** | published releases and test builds (prereleases) | `ctd-v<X.Y.Z>-test.<N>`, e.g. `ctd-v0.10.0-test.57` |

A test build defaults to **Test builds**, any other build to **Stable**. Change it in **Settings ▸ CTD ▸ Updates ▸ Update
channel**. Drafts are never offered.

Versions compare as semver: Major, Minor, Patch as numbers, then a stable `X.Y.Z` is newer than its own test builds
`X.Y.Z-test.N`, and test builds of the same `X.Y.Z` compare by `N`. So after `ctd-v0.10.0` is published, bump
`CTD-VERSION` (e.g. to `0.10.1`) on `ctd/main`; otherwise the next test builds (`0.10.0-test.N`) sort **before** the
stable `0.10.0` and Test builds users are offered the stable release again instead of them.

The build knows its channel and test number from CI (`CTD_OBS_CHANNEL` / `CTD_OBS_TEST_NUMBER`, read by
`frontend/cmake/ctd-version.cmake`). The title bar, About and `--version` show e.g. `0.10.0-test.57 (5653d30)` for a
test build and `0.10.0` for a stable release. A local build has no channel and compares as `X.Y.Z`.

## In the app

### At launch

1. A CTD OBS **splash screen** appears (CTD Night look: icon, wordmark, version, "Test build N" on test builds, a
   thin progress bar and a status line: **Checking for updates…**, **Loading plugins…**, **Loading scenes…**,
   **Starting…**). It closes when the main window is up.
2. **Before** the main window, plugins and scenes load, CTD OBS asks GitHub for the newest release on its channel, with
   a hard **3 s** limit. Offline, DNS or TLS failure, HTTP error, rate limit, timeout, bad JSON: startup simply goes on
   and the log gets one line, e.g. `[CTD] update check: timed out (no answer within 3000 ms), starting CTD OBS 0.10.0`.
3. If a newer version exists, the update dialog appears right there (version, release notes, **Download & Install**,
   **Skip This Version**, **Later**):
   - **Download & Install** downloads with a progress window, checks the SHA-256 against the release's
     `SHA256SUMS.txt` (a mismatch deletes the file), then installs (below) **without loading the main window**.
   - **Skip This Version**: that version isn't offered at launch again (a newer one is). **Later**: asked again at the
     next launch. Both continue the normal startup.
4. A "nothing new" answer is reused for **1 hour** (same version and channel), so quick relaunches don't ask GitHub
   again (its anonymous limit is 60 requests an hour per IP address).

No splash and no check for automated launches: `--startstreaming`, `--startrecording`, `--startreplaybuffer`,
`--startvirtualcam`, `--minimize-to-tray`. No check (the splash still shows) in safe mode, with `--disable-updater`,
after an unclean shutdown (crash recovery) and in a second instance started while CTD OBS is already running (it
can't tell whether the first one is live). **Settings ▸ CTD ▸ Updates ▸ Check for updates when CTD OBS starts** turns
the check off.

### While running

**CTD ▸ Check for Updates…** checks at any time (errors and "up to date" are shown; a skipped version is offered
anyway). **Never while live**: while any output is active (streaming, recording, replay buffer, virtual camera, the
vertical canvas's outputs) nothing is shown or installed; the notice waits until they have all stopped, and outputs
are checked again right before installing. Installing from a running CTD OBS asks **Close & Install** first, then
closes CTD OBS the normal way (scene collection and settings saved).

### Installing

- **Windows**: the installer `CTD-OBS-Setup-<version>-x64.exe` is downloaded to `%TEMP%\ctd-obs-update`, started
  (Windows asks for administrator rights) and CTD OBS quits. The installer waits up to ~30 s for CTD OBS to have
  exited (then asks to close it) before it copies files. The temp folder is cleared at the next start.
- **macOS**: `ctd-obs-<version>-macos-arm64.zip` is downloaded, checked, unzipped with `ditto -x -k` and checked again
  (one app, bundle id `com.cinetechdigital.ctd-obs`, executable present). **Signature:** once the running CTD OBS is
  signed with a Developer ID (`docs/ctd/macos-signing.md`), the new app must pass `codesign --verify --deep --strict`
  and carry the same Team ID, or it isn't swapped in (the manual steps are shown instead); an ad hoc build only logs
  the `codesign --verify` result. CTD OBS quits and a small helper script (started detached, `/bin/sh` with every path
  passed as an argument) waits until CTD OBS and anything else running from its bundle has exited. It then copies the
  new app next to the old one (`.ctd-obs-update-new.app`, the old app untouched while it copies), strips
  `com.apple.quarantine`, and swaps them with two renames in the same folder (old one to `.ctd-obs-update-old.app`, new
  one to the app's name), so there is never a half-copied app. The old app goes to the Trash and the new one opens.
  If any step fails the old app stays (or is put back) and is opened again; if CTD OBS was opened again meanwhile,
  nothing is changed. Its log is copied into the next CTD OBS log (`[CTD] Update helper: …`). The dialog warns that
  an ad hoc build may make macOS ask for Camera, Microphone and Screen Recording again (a Developer ID-signed one keeps
  them). If CTD OBS can't replace itself (the app's folder isn't writable, it runs from App Translocation or not from
  an app bundle, or the signature check failed), the zip is moved to Downloads and shown in Finder with the manual
  steps ([macos-run.md](./macos-run.md)).
- Everything is logged with the `[CTD]` prefix.

### Testing offline launch

Unit tests (`test/ctd-update`) run the launch check against fetches that fail (HTTP 403, rate-limit JSON, broken
JSON, an exception) and that never answer (one that honours cancel, one stuck where it can't), and assert the caller
gets its answer within the time limit. By hand: block `api.github.com` (e.g. turn Wi-Fi off, or an `/etc/hosts` entry
`0.0.0.0 api.github.com`), clear the cache (`UpToDateAt` in the `[CTDUpdates]` section of `user.ini`), start CTD OBS:
the splash shows **Checking for updates…** for at most ~3 s, then **Loading plugins…**, and the log has one
`[CTD] update check: failed (…)` or `timed out` line.

## Privacy

One anonymous request per launch, at most once an hour while nothing new turns up (plus each **Check for Updates…**):

```
GET https://api.github.com/repos/ItsLimitlezz/ctd-obs/releases?per_page=50
User-Agent: CTD-OBS/<version>
Accept: application/vnd.github+json
X-GitHub-Api-Version: 2022-11-28
```

No token, no cookies, no IDs, nothing about the user, the computer, the show or the Relay. GitHub sees the IP address,
as with any web request. The downloads (only after **Download & Install**) go to `github.com` release asset URLs the same
way. Turn the launch check off in **Settings ▸ CTD ▸ Updates**; then nothing is sent unless **Check for Updates…** is
used.

## How releases are made

`.github/workflows/ctd-build.yaml` (workflow **CTD OBS**) runs on every push to `ctd/**` and on `ctd-v<X.Y.Z>` tags. It
builds Windows (`ctd-windows.yaml`) and macOS (`ctd-macos.yaml`), runs the unit tests, then its **Release** job collects
both platforms' builds into one payload:

```
CTD-OBS-Setup-<version>-x64.exe
ctd-obs-<version>-windows-x64.zip
ctd-obs-<version>-macos-arm64.zip     (the native Apple Silicon build, else the Intel-host one)
SHA256SUMS.txt
```

| Trigger | Result |
|---|---|
| Push to `ctd/main` | **Prerelease** "Test build `<CTD-VERSION>-test.<N>`", tag `ctd-v<CTD-VERSION>-test.<N>` (N = the workflow's run number), public right away. The newest 10 test builds are kept; older ones and their tags are deleted. |
| Push of tag `ctd-v<X.Y.Z>` | **Draft** release `ctd-v<X.Y.Z>`. The tag must equal `ctd-v` + `CTD-VERSION`. The Head writes the notes and **publishes it after the SR check** (hub rule: SR checks every prod ship); until then nobody is offered it. |
| Push to any other `ctd/**` branch | Dry run: the payload is assembled and its `SHA256SUMS.txt` and notes uploaded as the `ctd-obs-release-manifest-<commit>` artifact; nothing is published. |

Release tags are always `ctd-v…`, never a bare `0.10.0`, which would clash with OBS Studio's own tags. OBS's version
(`git describe`) skips `ctd-*` tags (`cmake/common/versionconfig.cmake`, `.github/scripts/Package-Windows.ps1`).

The **Release** job uses only `GITHUB_TOKEN` with `contents: write`. The repository must be public (it is), so the
assets download without a GitHub login. Never rename `ctd-build.yaml`: its run number is the test number, and a
renamed workflow starts again at 1.

### Making a stable release

1. On `ctd/main`, `CTD-VERSION` holds the version to ship, e.g. `0.11.0`.
2. Tag that commit and push the tag: `git tag ctd-v0.11.0 && git push origin ctd-v0.11.0`.
3. Workflow **CTD OBS** builds it and creates the draft release `ctd-v0.11.0` with all assets.
4. SR check on the draft's assets. The Head writes the release notes (they are what the update dialog shows) and
   publishes the draft.
5. Bump `CTD-VERSION` on `ctd/main` to the next version (e.g. `0.11.1`).
