# Running CTD OBS on a Mac (Apple Silicon)

Test builds of CTD OBS for Apple Silicon Macs (M1 and later) come from GitHub Actions (`.github/workflows/ctd-macos.yaml`,
every push to a `ctd/**` branch). Once CTD's signing secrets are set they are **signed with CineTech Digital's Developer
ID and notarized by Apple** (`docs/ctd/macos-signing.md`): they install like any Mac app. The run's summary says
**Developer ID, notarized** or **ad hoc, not notarized**; an ad hoc build needs one Terminal command before the first
launch (step 2.3). CTD OBS runs **beside OBS Studio**: its own app, its own settings folder
(`~/Library/Application Support/ctd-obs`), its own permissions. Nothing of OBS Studio's is read or changed.

Needs macOS 13 or later on an Apple Silicon Mac.

## 1. Download

1. On the Mac, sign in to GitHub and open the build: **github.com/ItsLimitlezz/ctd-obs → Actions → CTD OBS macOS**, then
   the run for the commit you want (green tick).
2. At the bottom of the run page, under **Artifacts**, click **`ctd-obs-macos-arm64-<commit>`**. A `.zip` downloads.
   A signed build also has a disk image in it (`ctd-obs-<version>-<commit>-macos-arm64.dmg`): you can open that instead
   of the inner zip in step 3 and drag **CTD OBS** onto **Applications** in its window.
   `ctd-obs-macos-arm64-<commit>-intel-host` is the same Apple Silicon app, built on an Intel runner when GitHub has no
   Apple Silicon runner free; either one is fine.
3. In Downloads, double-click the zip (Safari may already have opened it into a folder). Inside is a second zip,
   `ctd-obs-<version>-<commit>-macos-arm64.zip`: double-click it too. You get **CTD OBS** (`CTD OBS.app`).

   The inner zip is on purpose: it keeps the app's internal links and permissions, which GitHub's own zip would break.
   Always unzip with Finder (Archive Utility), not a third-party unzipper.

## 2. Install

1. Quit CTD OBS if an older test build is running.
2. Drag **CTD OBS** into **Applications** (replace the old one if asked).
3. **Only for an ad hoc build** (summary says "ad hoc, not notarized"): open **Terminal** and run:

   ```sh
   xattr -dr com.apple.quarantine "/Applications/CTD OBS.app"
   ```

   This removes the "downloaded from the internet" mark. Without it macOS says the app "is damaged and can't be opened"
   (that only means it isn't notarized). A signed, notarized build needs none of this.

## 3. First launch

1. In Applications, double-click **CTD OBS**. A signed build asks once whether to open an app downloaded from the
   internet: **Open**. An ad hoc build: **right-click CTD OBS → Open**, then **Open** in the dialog; later launches are
   a normal double-click.
   - If macOS still refuses an ad hoc build: **System Settings → Privacy & Security**, scroll down, **Open Anyway**
     next to CTD OBS.
2. CTD OBS asks for permissions in its own window. Grant, for **CTD OBS**:
   - **Camera** (camera sources) and **Microphone** (audio inputs): allow in the prompt.
   - **Screen & System Audio Recording** (display/window capture): opens System Settings; switch **CTD OBS** on, then
     **quit and reopen CTD OBS** (macOS applies it only on restart).
   - **Accessibility** is only for global hotkeys; skip it unless you use them.

   OBS Studio's permissions don't carry over: CTD OBS is a separate app (`com.cinetechdigital.ctd-obs`). **Signed
   builds keep these permissions across updates** (they belong to CTD's Developer ID and the bundle id). An **ad hoc
   build may ask again** after each update (its signature changes per build); if a source is black after an update,
   switch CTD OBS off and on again in that Privacy & Security list, then restart CTD OBS. Going from an ad hoc build to
   the first signed one counts as a change, so it may ask once more then.
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

- **No virtual camera.** On macOS it needs a camera system extension with its own Developer ID provisioning profile
  (what's needed: `docs/ctd/macos-signing.md`). The Start Virtual Camera button isn't shown. (OBS Studio's virtual
  camera, if installed, is unaffected.)
- **No auto-update** (same as Windows): install each test build by hand, steps 1–2.
- Ad hoc builds (no signing secrets, e.g. from a fork) aren't notarized: the `xattr` step is needed for every new one.
- Builds are never launched on GitHub's Macs (their virtual Macs crash OBS at start for an unrelated graphics reason),
  so **each build's first real launch is on your Mac**.

## Where things are

| What | Where |
|---|---|
| Settings, scenes, profiles | `~/Library/Application Support/ctd-obs/` (`basic/scenes`, `basic/profiles`) |
| Logs | **Help → Log Files → Show Log Files**, or `~/Library/Application Support/ctd-obs/logs/` |
| Third-party plugins (if ever) | `~/Library/Application Support/ctd-obs/plugins/` (OBS Studio's plugins are not loaded) |
| Remove completely | Delete `/Applications/CTD OBS.app` and `~/Library/Application Support/ctd-obs/` |
