# Running CTD OBS on a Mac (Apple Silicon)

CTD OBS for Apple Silicon Macs (M1 and later) is published on the repository's GitHub Releases: a test build for every
push to `ctd/main`, stable releases once they pass the SR check ([updates.md](./updates.md)). The builds are **signed ad
hoc, not notarized** (there is no Developer ID yet), so macOS needs one Terminal command before the first launch. CTD OBS runs **beside OBS Studio**: its own app, its own settings
folder (`~/Library/Application Support/ctd-obs`), its own permissions. Nothing of OBS Studio's is read or changed.

Needs macOS 13 or later on an Apple Silicon Mac.

## 1. Download

Once CTD OBS is installed, **CTD ▸ Check for Updates…** does this step for you: it downloads the new zip to Downloads,
checks its SHA-256 and shows it in Finder. Continue at item 3 below, then **2. Install**.

1. On the Mac open **github.com/ItsLimitlezz/ctd-obs/releases** (no GitHub login needed). Pick the newest release
   (**Latest**), or the newest **Test build** (marked **Pre-release**) if you're testing.
2. Under **Assets**, click **`ctd-obs-<version>-macos-arm64.zip`**. It downloads to Downloads. (`SHA256SUMS.txt` next to
   it holds the checksums: `shasum -a 256 ~/Downloads/ctd-obs-*-macos-arm64.zip` must print the same.)
3. In Downloads, double-click the zip (Safari may already have opened it into a folder). You get **CTD OBS**
   (`CTD OBS.app`). Always unzip with Finder (Archive Utility), not a third-party unzipper: it keeps the app's internal
   links and permissions.

   A build for a branch other than `ctd/main` is only on GitHub Actions (sign in, **Actions → CTD OBS**, the run, under
   **Artifacts** `ctd-obs-macos-arm64-<commit>`). That download is a zip **inside** a zip: double-click both.

## 2. Install

1. Quit CTD OBS if an older test build is running.
2. Drag **CTD OBS** into **Applications** (replace the old one if asked).
3. Open **Terminal** and run:

   ```sh
   xattr -dr com.apple.quarantine "/Applications/CTD OBS.app"
   ```

   This removes the "downloaded from the internet" mark. Without it macOS says the app "is damaged and can't be opened"
   (that only means it isn't notarized).

## 3. First launch

1. In Applications, **right-click CTD OBS → Open**, then **Open** in the dialog. Later launches are a normal
   double-click.
   - If macOS still refuses: **System Settings → Privacy & Security**, scroll down, **Open Anyway** next to CTD OBS.
2. CTD OBS asks for permissions in its own window. Grant, for **CTD OBS**:
   - **Camera** (camera sources) and **Microphone** (audio inputs): allow in the prompt.
   - **Screen & System Audio Recording** (display/window capture): opens System Settings; switch **CTD OBS** on, then
     **quit and reopen CTD OBS** (macOS applies it only on restart).
   - **Accessibility** is only for global hotkeys; skip it unless you use them.

   OBS Studio's permissions don't carry over: CTD OBS is a separate app (`com.cinetechdigital.ctd-obs`). A **new test
   build may ask again** (ad hoc signatures change per build); if a source is black after an update, switch CTD OBS off
   and on again in that Privacy & Security list, then restart CTD OBS.
3. **Welcome to CTD OBS** appears on the first run: choose **Later** for now if you're importing your OBS setup first
   (step 4), or **Pair with CTD Relay** to go straight to step 5.

## 4. Bring over your OBS Studio scenes and profile

Do this before pairing so **Send stream** writes into the profile you'll actually use. OBS Studio's files stay where
they are; CTD OBS imports copies.

In the file dialogs, `~/Library` is hidden: press **⌘⇧G** and paste the path.

**Scene collection**

1. **Scene Collection → Import Scene Collection…**
2. **Browse…** (or drag the file into the window) and pick your collection in
   `~/Library/Application Support/obs-studio/basic/scenes/` (one `.json` per collection; ignore `.bak` files).
3. Tick it, **Import**.
4. **Scene Collection →** pick the imported collection.

**Profile**

1. **Profile → Import Profile**
2. Pick the profile's **folder** in `~/Library/Application Support/obs-studio/basic/profiles/` (e.g. `…/profiles/Show`)
   and **Open**.
3. **Profile →** pick the imported profile.

Check **Settings → Output** and **Settings → Video** look like OBS Studio's. Sources that point at files (images,
media, LUTs) keep their paths, so they work on the same Mac.

Alternative with both apps closed: copy the `.json` files into `~/Library/Application Support/ctd-obs/basic/scenes/`
and the profile folders into `~/Library/Application Support/ctd-obs/basic/profiles/`, then open CTD OBS and pick them
in the menus.

## 5. Pair the CTD Relay dock

1. If the **CTD Relay** dock isn't showing: **CTD → CTD Relay**.
2. Type the prod Relay's address (the same one the PC uses) in the dock's address box, **Connect**.
3. **Open the approval page →**, approve **this** pairing code in the Relay web UI. The dock shows it's connected.
4. **This PC streams to** (in the dock): pick the Relay profile for the show.

## 6. Send stream and test-mode check

1. In the dock, **Send CTD OBS's stream to this profile →**. This sets up **both** the main stream and the vertical
   canvas's stream to that Relay profile (keyframe interval 2 s), no copying of keys.
2. **Test-mode check before the show:** put the Relay profile in **test mode** in the Relay web UI. The dock's session
   line shows a **TEST** badge.
3. **Start Streaming** in CTD OBS (and the vertical canvas's stream if it doesn't start with it). In the Relay web UI
   check that the profile receives **main and vertical** and the preview looks right. In test mode nothing reaches the
   real platforms (except Twitch Dual Format, which the dock warns about: keep it off).
4. **Stop Streaming** (both). When you're ready for the real show, turn test mode off in Relay, then start streaming.

## What is and isn't in the macOS build

In: CTD Relay and CTD Hub docks, Send stream (main + vertical), the vertical canvas, Show Profiles, the CTD menu and
Settings ▸ CTD, CTD Night theme, browser sources, WebSocket server.

Not in, or untested:

- **No virtual camera.** On macOS it needs a camera system extension signed with a Developer ID, which CTD doesn't
  have yet. The Start Virtual Camera button isn't shown. (OBS Studio's virtual camera, if installed, is unaffected.)
- **No automatic replacement**: **CTD ▸ Check for Updates…** downloads and checks a new build, but installing it is
  sections 1–2 by hand until CTD OBS is notarized ([updates.md](./updates.md)).
- Not notarized: the `xattr` step is needed for every new build.
- Builds are never launched on GitHub's Macs (their virtual Macs crash OBS at start for an unrelated graphics reason),
  so **each build's first real launch is on your Mac**.

## Where things are

| What | Where |
|---|---|
| Settings, scenes, profiles | `~/Library/Application Support/ctd-obs/` (`basic/scenes`, `basic/profiles`) |
| Logs | **Help → Log Files → Show Log Files**, or `~/Library/Application Support/ctd-obs/logs/` |
| Third-party plugins (if ever) | `~/Library/Application Support/ctd-obs/plugins/` (OBS Studio's plugins are not loaded) |
| Remove completely | Delete `/Applications/CTD OBS.app` and `~/Library/Application Support/ctd-obs/` |
