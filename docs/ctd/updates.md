# CTD OBS updates

CTD OBS checks GitHub Releases of this repository for a newer CTD OBS and offers to install it. OBS Studio's own
updaters stay off: the Windows updater (which points at obsproject.com) is disabled in `frontend/obs-main.cpp`, and the
macOS build has no Sparkle (`ctd-macos.yaml` fails if it is bundled).

Code: `frontend/ctd/ctd-update.cpp` (Qt), `frontend/ctd/ctd-update-logic.cpp` (version compare and release parsing, unit
tests in `test/ctd-update`). CI: `.github/workflows/ctd-build.yaml`.

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

- **Settings ▸ CTD ▸ Updates**: **Check for updates automatically** (on by default) and **Update channel**.
- **CTD ▸ Check for Updates…**: checks now. Errors and "up to date" are shown; a skipped version is offered again.
- Automatic check: about 10 s after CTD OBS starts, at most once every 24 h. Network errors are only logged.
- When an update is found: a dialog with the version, its release notes and **Download & Install**, **Skip This
  Version** (no automatic offer for that version again) and **Later** (asked again at the next automatic check).
- **Never while live**: while any output is active (streaming, recording, replay buffer, virtual camera, the vertical
  canvas's outputs) nothing is shown or installed. The notice waits and appears once they have all stopped. A manual
  check during a show only says that an update is waiting.
- **Windows**: downloads `CTD-OBS-Setup-<version>-x64.exe` to `%TEMP%\ctd-obs-update`, checks its SHA-256 against the
  release's `SHA256SUMS.txt` (a mismatch deletes it), asks **Close & Install**, starts the installer (Windows asks for
  administrator rights) and closes CTD OBS the normal way, which saves the scene collection and settings. The installer
  waits for CTD OBS to have exited before it copies files. The temp folder is cleared at the next start.
- **macOS**: downloads `ctd-obs-<version>-macos-arm64.zip` to Downloads, checks its SHA-256, shows it in Finder and
  lists the install steps (unzip, drag to Applications, the `xattr` command with a Copy button, right-click ▸ Open; see
  [macos-run.md](./macos-run.md)). There is no automatic replacement while the app is only signed ad hoc. TODO: with a
  Developer ID signed and notarized build, replace `/Applications/CTD OBS.app` automatically.
- Everything is logged with the `[CTD]` prefix (`[CTD] Update check …`).

## Privacy

One anonymous request, at most once a day (plus each **Check for Updates…**):

```
GET https://api.github.com/repos/ItsLimitlezz/ctd-obs/releases?per_page=50
User-Agent: CTD-OBS/<version>
Accept: application/vnd.github+json
X-GitHub-Api-Version: 2022-11-28
```

No token, no cookies, no IDs, nothing about the user, the computer, the show or the Relay. GitHub sees the IP address,
as with any web request. The downloads (only after **Download & Install**) go to `github.com` release asset URLs the same
way. Turn the automatic check off in **Settings ▸ CTD ▸ Updates**; then nothing is sent unless **Check for Updates…** is
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
