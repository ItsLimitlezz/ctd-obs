# NDI® in CTD OBS

NDI® is a registered trademark of Vizrt NDI AB. More about NDI: <https://ndi.video/>.

CTD OBS **sends** NDI. It doesn't receive it. You can send:

- the program ("CTD OBS Program")
- the vertical canvas ("CTD OBS Vertical")
- any source, scene or group, each as its own NDI sender (the **NDI® Output** filter)

It works on macOS and Windows, without putting anything system-wide on the machine and without being able to take a
live show down with it. Juan, 2026-11-23: "our only role in this software is adding support to export NDI; we should be
able to select individual sources and send it out."

**Why this design:** on 2026-11-22 Juan's NDI install (NDI Tools + Runtime + DistroAV, which also installed
CoreMediaIO DAL camera plugins and the `NDIAudio.driver` / `NDIAudioOut.driver` CoreAudio HAL drivers) made CTD OBS on
macOS leak about 1 GB a minute and crash during a live. Removing all of it fixed it (hub: `lessons/ctd-obs.md`). Every
rule below exists so that can't happen again.

## Rules

1. **No system pieces.** No DAL/CMIO camera plugins, no HAL audio drivers, no NDI Tools, no virtual camera or audio
   device, no DirectShow filters registered by CTD OBS. Only the NDI library, loaded into CTD OBS's own process, to
   send.
2. **One exact, verified NDI library.** CTD OBS loads only its own copy of an NDI library whose SHA-256 is on the
   allow-list compiled into the build (`plugins/ctd-ndi/src/ndi-allowlist.cpp`). It never `dlopen`s / `LoadLibrary`s a
   file in `/usr/local/lib`, `/Library/NDI…`, Program Files, `PATH` or `NDI_RUNTIME_DIR_V6`, so another NDI install
   (or an update of it) can't change what runs inside CTD OBS. See [Where the library comes from](#where-the-library-comes-from).
3. **Off by default, a kill switch, a safe launch.**
   - Settings ▸ CTD ▸ NDI: **Off** (default) / **Send**. While Off, the NDI library isn't loaded at all.
   - `--ctd-no-ndi` (and OBS's own `--safe-mode`) start CTD OBS with NDI off for that launch, whatever the setting.
   - **CTD ▸ NDI ▸ Start without NDI…** restarts that way, and **CTD ▸ NDI ▸ Pause NDI** stops every sender at once.
4. **Guardrails:**
   - at most 8 NDI senders at once, all kinds together
   - no frame queue that can grow
   - a sender works only while its source is on air, and nothing is rendered for a sender that isn't running
   - a memory watchdog
   - every NDI error is caught and logged, and a sender that fails just stops, with the reason shown

## Licence finding

Read from the NDI SDK 6.4.0.2 itself (`NDI SDK License Agreement`, `NDI SDK Documentation.pdf` §3–4 and §23, shipped
in the SDK downloaded 2026-11-22) and docs.ndi.video.

**What NDI allows.** NDI's terms *allow* shipping the library inside an application, and even recommend it:

- SDK Documentation §4.2: binary files "You may distribute these files within your application if your EULA terms
  cover the specific requirements of the NDI SDK EULA, and your application covers the terms of the License section".
- §3: "You should include the NDI DLLs as part of your own application and keep them in your application folders …
  Please do not install your NDI DLLs into the system path".
- Conditions:
  - Ship `Processing.NDI.Lib.Licenses.txt` (macOS: `libndi_licenses.txt`) beside it (§23).
  - The product's EULA must prohibit modifying or reverse engineering the SDK and must disclaim NDI's warranties
    (Licence §3d).
  - Ship "the latest version of the SDK available at the time of … distribution" (Licence §2b, and §4.3 "keep the
    versions you distribute up to date").
- Re-signing the dylib with CTD's Developer ID (needed for the hardened runtime and notarization inside
  `Contents/Frameworks`) isn't addressed anywhere. The licence forbids "modifications" without defining whether a new
  code signature is one.

**Why CTD OBS doesn't bundle it anyway.** CTD OBS is GPL-2.0 (OBS Studio's licence), and the NDI library is
proprietary.

- Licence §3d requires the product's licence to add restrictions (no modification, no reverse engineering) that the
  GPL forbids adding (GPL-2.0 §6). Once loaded into OBS, the NDI code and GPL code form one program.
- The OBS Project's own position, in DistroAV issue #465: "The runtime can't be distributed with this plugin legally.
  It has to be the user's responsibility to get it on their own." (DistroAV issues #4 and #230 have the same
  discussion.) FFmpeg treats libndi as "nonfree" for the same reason.
- NDI's terms do explicitly allow what open-source projects do instead: "open-source projects have the right to
  include the header files within their distributions, which may then be used with dynamic loading of the NDI
  libraries" (§4; the headers are MIT).

**Outcome: not bundled.** Bundling would satisfy NDI's licence, but **it isn't clear that a GPL-2.0 app may ship it**,
and the people who own OBS's copyright say no. So CTD OBS uses the fallback from the brief, made stricter:

- CTD OBS ships only NDI's MIT-licensed headers (`deps/ndi/include`).
- The user installs the official **NDI 6 Runtime** (not NDI Tools).
- CTD OBS accepts the Runtime's library only if its SHA-256 is on the allow-list and refuses any other file. It then
  loads its own verified copy (below), never the installed file.

The bundled variant is built in but switched off (CMake option `CTD_NDI_BUNDLED`, default `OFF`). Turning it on needs
two things from Juan:

1. A written answer from Vizrt (licensing@ndi.video) on GPL distribution and on re-signing.
2. CTD's own legal call.

Until then, keep it off.

**Attribution** (SDK Documentation §3) is done the same either way:

- "NDI® is a registered trademark of Vizrt NDI AB" appears in Help ▸ About, in Settings ▸ CTD ▸ NDI (next to the first
  use) and at the top of this document.
- A link to <https://ndi.video/> sits next to every place NDI is chosen: Settings ▸ CTD ▸ NDI, the NDI Source's
  properties, and this document.
- NDI is used only to say CTD OBS works with NDI: never in CTD OBS's name, and nothing suggests Vizrt sponsors it
  (Licence §3f).
- No NDI Tools are distributed or installed (§3).
- The NDI SDK FAQ says the standard SDK is free for non-commercial use, while the 6.4 manual says "free or commercial
  Products … without paying any license fees". Vizrt should confirm commercial use in the same email.

### What the NDI installers put on the system (checked 2026-11-22)

| Installer | What it installs |
|---|---|
| **NDI 6 Runtime, macOS** (`ndi.link/NDIRedistV6Apple` → `libNDI_for_Mac.pkg`, 6.4.0.2) | `/usr/local/lib/libndi.dylib` (universal arm64 + x86_64, signed by NewTek) and `libndi_licenses.txt`. Nothing else, no scripts |
| **NDI 6 Runtime, Windows** (`ndi.link/NDIRedistV6` → `NDI 6 Runtime.exe`, 6.4.0.2) | `Processing.NDI.Lib.x64.dll` and friends in Program Files, `NDI_RUNTIME_DIR_V6`, and it **registers NDI's DirectShow filters** (SDK Documentation §22). They are passive COM filters that CTD OBS never asks for; still, they are system-wide |
| **NDI Tools** (either platform) | The incident: DAL camera plugin `NDIVideoOut.plugin`, HAL drivers `NDIAudio.driver` / `NDIAudioOut.driver`, a LaunchDaemon, the HX driver, apps. **Never install NDI Tools on a show machine** |

Because CTD OBS keeps its own copy of the library, the Runtime is only needed **once**, to set NDI up. It can be
uninstalled afterwards; on Windows that also removes the DirectShow filters. CTD OBS keeps working from its copy.

## Where the library comes from

`plugins/ctd-ndi/src/ndi-loader.cpp`, policy in `ndi-logic.cpp` (unit-tested in `test/ctd-ndi`).

1. **Set up** (Settings ▸ CTD ▸ NDI ▸ **Set up from NDI Runtime**, also tried automatically the first time NDI is
   switched on): CTD OBS looks for the Runtime's library only where the official installer puts it
   (`RuntimeLibraryCandidates`):
   - macOS: `/usr/local/lib/libndi.dylib`, then `$NDI_RUNTIME_DIR_V6/libndi.dylib` if that is set
   - Windows: `%NDI_RUNTIME_DIR_V6%\Processing.NDI.Lib.x64.dll` if set, then
     `%ProgramFiles%\NDI\NDI 6 Runtime\v6\…` and `%ProgramFiles%\NDI\NDI 6 Runtime\…`

   It hashes the file **without loading it**. The SHA-256 must match an allow-list entry for this platform, or
   nothing happens ("The installed NDI Runtime isn't a version approved for CTD OBS").

   A match is copied, with NDI's licence file, to CTD OBS's own folder:
   - macOS: `~/Library/Application Support/ctd-obs/plugin_config/ctd-ndi/runtime/<version>/`
   - Windows: `%APPDATA%\ctd-obs\plugin_config\ctd-ndi\runtime\<version>\`

   The copy is hashed again before it's kept. Copying is done by the user's own CTD OBS on their own machine; it is
   not redistribution.
2. **Load** (at launch when NDI is on, or when it's switched on): only that copy is loaded.
   - It is hashed again first. A changed file is refused and deleted, and Set up runs again.
   - It is loaded with `dlopen(RTLD_NOW | RTLD_LOCAL)` / `LoadLibraryExW(LOAD_WITH_ALTERED_SEARCH_PATH)` by full path.
   - Then `NDIlib_v6_load`, `NDIlib_version()` (must contain the allow-listed version) and `NDIlib_initialize()`.
   - Any failure: NDI stays off for this launch with the reason in Settings and the log. Nothing crashes.
3. **Never:** the loader has no search path. `LoadPathAllowed()` (with `PathIsForbidden()`) refuses any load path in the system
   locations, and the unit tests check that every path the loader can produce is in CTD OBS's own folder (or, for
   `CTD_NDI_BUNDLED`, the app bundle). The forbidden locations are:
   - macOS: all of `/usr` (`/usr/lib`, `/usr/local/lib`, Homebrew on Intel), `/opt` (Homebrew, MacPorts), `/Library`
     (`/Library/NDI…`, DAL, HAL, Frameworks), `/System`, `/bin`, `/sbin`
   - Windows: `%ProgramFiles%\NDI`, `%ProgramFiles(x86)%\NDI` (also `C:\Program Files\NDI` whatever the variables say),
     `%SystemRoot%`
   - `NDI_RUNTIME_DIR_V6`

   Nothing is ever looked up through `PATH` or the DLL search path: the one load is by full path.
4. **Bundled build** (`CTD_NDI_BUNDLED=ON`, off for now): the only path is `CTD OBS.app/Contents/Frameworks/libndi.dylib`
   or `Processing.NDI.Lib.x64.dll` next to `ctd-ndi.dll`. The file is signed with the app, so it isn't hashed; it is
   still version-checked.

**The allow-list** is `{platform, NDI version, SHA-256 of the library}`:

| Platform | NDI version | File | SHA-256 |
|---|---|---|---|
| macOS (universal) | 6.4.0.2 | `libndi.dylib` from `libNDI_for_Mac.pkg` | `b10315f9435c59a5db76dd44d27ea809432468151aea458359129f279ca6f09c` |
| Windows x64 | 6.4.0.2 | `Processing.NDI.Lib.x64.dll` from `NDI 6 Runtime.exe` | `adca7e9c98ddddb01a1b684f90c97d0bd764990ee2839524fde51ab37f21028a` |

**Adding a version** (when Vizrt ships a new Runtime; NDI wants us reasonably current):

1. Download the new Runtime, extract the library and hash it (`shasum -a 256`).
2. Add the entry to `ndi-allowlist.cpp` and this table, and pass the soak test with it.
3. Then ship.

Old entries stay, so machines already set up keep working. Remove an entry only when it's known bad.

**Which version a machine ends up on.** Machines keep the version they were set up with, even when the system
Runtime is updated or removed.

- A machine set up on a newer Runtime that isn't approved yet gets "isn't approved for CTD OBS" and NDI stays off.
- To run another approved version, use Set up again: it switches to the newest approved copy it finds.

## The module: plugins/ctd-ndi (not DistroAV)

DistroAV 6.2.2 ("OBS 33+ ready", commit `9b1635f`) was the candidate. It can't meet these rules without rewriting
most of it.

| Rule | DistroAV 6.2.2 |
|---|---|
| Never load a system NDI | `load_ndilib()` searches `NDI_RUNTIME_DIR_V6`, `/usr/lib`, `/usr/lib64`, `/usr/local/lib`; Windows only the env var; no allow-list |
| No hooks into the window / preview | `preview-output.cpp` uses `obs_add_main_render_callback` with `gs_stage_texture` + `gs_stagesurface_map` in the same frame (a GPU stall on every frame); its per-source "NDI filter" stages and maps in the same frame too; `main-output.cpp` and its Tools menu dialog are tied to the main window |
| No install prompts / network | ERR dialogs at startup; buttons that run `winget install NDI.NDIRuntime` / `brew reinstall libndi` in a shell; an update check POSTing the OBS install GUID and the plugin's hash to distroav.org |
| libndi not loaded until enabled | Loaded unconditionally in `obs_module_load` |
| Bounded queues, cap on senders | No cap; the sender passes OBS's frame to `send_send_video_async_v2` without a copy (buffer lifetime race) |
| Built in-tree against our 33.0.1 headers | An obs-plugintemplate project (`buildspec.json` obs-studio 31.1.1, its own `project()`, `find_package(libobs)`); no in-tree build; needs Qt Network and libcurl |

Patching it meant a CTD fork of DistroAV changed in almost every file (the loader, plugin-main, config, both outputs,
the filter, forms, update, CMake), kept in sync with upstream by hand. A small send-only module of our own is less
code to review than that patch set, and it's easier to keep to these rules.

- DistroAV is **not** the supported route, and isn't in the About credits. `docs/ctd/plugins.md` says the same.
- Its code was read for reference only; nothing is copied from it.

**`ctd-ndi`** is a core module: GPL-2.0-or-later, Windows and macOS, built in-tree in this repository's CI against
OBS 33.0.1 and signed with the app like the other modules. It needs no Qt, curl or frontend window code. Its parts:

| Part | What it does |
|---|---|
| `ndi-plugin.cpp` | Module load: mode, the program and vertical senders, procs for the app; loads NDI only when enabled; stops every sender around profile and scene collection changes and on exit |
| `ndi-loader.cpp` | Set up and load, as above |
| `ndi-sender.cpp` | One NDI sender: the bounded video queue, the sender thread, audio, unique names |
| `ndi-output.cpp` | The raw outputs that feed senders: `ctd_ndi_output` (program, vertical: video + audio) and `ctd_ndi_source_output` (a per-source sender's video) |
| `ndi-filter.cpp` | **NDI® Output** (`ctd_ndi_filter`): the per-source sender |
| `ndi-logic.cpp/.hpp` | Mode parsing, allow-list matching, path policy, SHA-256, the bounded queue, the sender slots, sender names and the run decision: no OBS, no NDI, unit-tested |

Nothing in it touches the main window, the preview, a main render callback, a display or a Qt widget. Its only
frontend use is `obs_frontend_get_user_config` and frontend events.

### Program and vertical

- **The output:** `ctd_ndi_output` is an `OBS_OUTPUT_AV` raw output (no encoder, no render callback).
  - "CTD OBS Program" sends the main video and audio.
  - "CTD OBS Vertical" sends the canvas named `Vertical` (`obs_get_canvas_by_name`) with the main audio.
- **When it runs:** while NDI is on (and not paused), from when loading finishes until exit. Each can be ticked or
  unticked in Settings ▸ CTD ▸ NDI's outputs list (`[CTD] NDISendProgram`, default on; `[CTD] NDISendVertical`, default
  off).
- **Vertical resolution:** like any active output, the vertical send holds the vertical canvas's resolution. Stop it
  before changing the vertical size.

### Per-source senders: the NDI® Output filter

- **Adding one:** right-click any source, scene or group ▸ **Filters** ▸ **+** ▸ **NDI® Output**.
  - Its settings are the **NDI sender name** (empty: the source's name) and **Send the source's audio**.
  - It is saved with the scene collection like any filter.
  - Disabling the filter (the eye in Filters, or unticking it in Settings ▸ CTD ▸ NDI) stops the sender.
- **When it runs:** only while all of these hold (`DecideSender`, unit-tested). Otherwise its canvas, output and NDI
  sender don't exist, and **nothing is rendered for it**.
  1. NDI is on and not paused, and no profile / collection switch is under way
  2. the filter is enabled
  3. its source is **active**: on air in the program, or inside a scene or group that is (OBS's own `obs_source_active`)
  4. the source has a picture (or has no video at all: then it's an audio-only sender)
  5. the name is usable and no other running sender has it
  6. one of the 8 sender slots is free

  The first reason it doesn't run shows as its state, in the filter's properties and in the outputs list: "Waiting:
  the source isn't on air", "NDI sender limit reached", and so on.
- **Video:**
  - It uses a **private canvas** (`obs_canvas_create_private`, `EPHEMERAL`, no `ACTIVATE`: it never makes the source
    active itself, so "on air" stays the program's call), the size of the source, with the source on channel 0.
  - A `ctd_ndi_source_output` on that canvas's video gets OBS's own render, GPU colour conversion and asynchronous
    readback. That is the path the vertical canvas uses, not a hand-written texture readback, so there's no GPU stall.
  - The frames go into the sender's bounded queue.
  - A source bigger than 4096 px is scaled down to fit, keeping its aspect.
  - A size change restarts the sender at the new size.
- **Audio:** `filter_audio` sends the source's own audio as it passes through, unchanged. Scenes and groups have no
  audio of their own in OBS, so their senders are video only.
- **Threading:**
  - `video_tick` (graphics thread) only notices that something may have changed (a signature of the inputs) and queues
    the decision on the UI thread.
  - Canvases and outputs are created and destroyed only on the UI thread. A filter freed on another thread hands its
    canvas and output to the UI thread to free.
  - When a sender stops, the others re-check (a slot or a name may be free).

### Every sender

- **Video** (`ndi-sender.cpp`):
  - The raw output's callback (OBS's video thread) copies the frame into one of **3 preallocated buffers** and pushes
    it into a **bounded queue of 2**. When the queue is full, the oldest frame is dropped and its buffer reused.
  - A sender thread calls `NDIlib_send_send_video_v2`, which is synchronous, so NDI never reads a buffer that is
    reused.
  - Memory is fixed at 3 frames per sender, whatever NDI or the network does. Dropped frames are counted and logged.
  - NV12, I420, UYVY, BGRA, BGRX and RGBA go as they are. Other formats are converted to NV12 by OBS
    (`obs_output_set_video_conversion`).
  - The frame rate is the exact `fps_num/fps_den`.
- **Audio:** planar float, copied into one reusable buffer, sent with `NDIlib_send_send_audio_v3`.
- **Names:** the cleaned name (trimmed, no control characters, at most 63 bytes, never splitting a UTF-8 character)
  must be unique among running senders. A second sender with a taken name waits, with the state "Another sender
  already uses this name".
- **Cap:** at most **8** senders at once (program, vertical and per-source together). The 9th waits, with the state
  "NDI sender limit reached", and starts when a slot frees.
- **Switches and updates:**
  - Every sender stops on `PROFILE_CHANGING` / `SCENE_COLLECTION_CHANGING` and starts again after the change. A new
    collection brings its own NDI Output filters.
  - `OBSBasic::CtdAnyOutputActive` doesn't count `ctd_ndi_output` / `ctd_ndi_source_output`, so NDI never blocks a
    profile / collection switch or an update install.

### Mode and kill switch

| Mode (`[CTD] NDIMode` in user.ini) | NDI library | Senders |
|---|---|---|
| `off` (default; also anything unknown, and an old test build's `receive` / `receive_send`) | not loaded | none |
| `send` | loaded | program, vertical (as ticked), every enabled NDI Output filter whose source is on air |

- **Changing the mode** (Settings ▸ CTD ▸ NDI, OK/Apply) takes effect at once:
  - Going **to Off** stops every sender.
  - The library stays mapped until CTD OBS quits, because unloading it while NDI's own threads wind down isn't safe.
    Settings says "NDI is off. The library stays in memory, unused, until CTD OBS restarts."
  - Going **on** loads it if it isn't loaded yet.
- **Launching without NDI:** `--ctd-no-ndi` or OBS's `--safe-mode` starts with NDI off for that launch, whatever the
  setting; Settings shows "NDI is off for this launch". **CTD ▸ NDI ▸ Start without NDI…** asks, then restarts CTD OBS
  with `--ctd-no-ndi` (it warns first when streaming or recording, since a restart ends them).
- **CTD ▸ NDI ▸ Pause NDI** stops every sender at once, without restarting and without changing the setting. **Resume
  NDI** starts them again.

### Settings ▸ CTD ▸ NDI

- **NDI:** Off / Send.
- **NDI outputs:** one row per sender: CTD OBS Program, CTD OBS Vertical, then every NDI Output filter in the scene
  collection (on inputs, scenes and groups). Each row shows the sender name, the source and the state.
  - **Tick / untick** sends / stops: for program and vertical it's the setting, for a filter its enabled state.
  - **Double-click a filter's name** to rename its sender.
  - Changes apply on OK/Apply; **Refresh** reloads the states.
- **The library line:**
  - **Set up from NDI Runtime**
  - the link to the NDI 6 Runtime (not NDI Tools)
  - a warning when NDI Tools' system pieces are installed
- The trademark line and the ndi.video link.

### Memory watchdog

The watchdog is in the CTD OBS app, so it watches everything, not only NDI: `frontend/ctd/ctd-memory-watchdog.cpp`,
with the maths in `ctd-memory-watchdog-logic.cpp` (unit-tested).

- **What it measures** (every 10 s):
  - macOS: `task_info(TASK_VM_INFO).phys_footprint`, the number Activity Monitor shows. It includes IOSurfaces and
    graphics memory owned by the process.
  - Windows: private bytes (`PROCESS_MEMORY_COUNTERS_EX.PrivateUsage`) plus the process's local video memory on the
    main adapter (`IDXGIAdapter3::QueryVideoMemoryInfo(...).CurrentUsage`).
- **Log, every 60 s:** `[ctd-watchdog] footprint 1834 MB (+12 MB/min over 3 min), NDI send: 4 senders, 0 dropped`.
- **Warning when either holds:**
  - It grew by at least **200 MB in each of the last 3 one-minute intervals** (sustained, so one spike such as a scene
    load doesn't count).
  - It passed **6144 MB** (`[CTD] WatchdogLimitMB` in user.ini).

  The warning is non-blocking (a window that doesn't stop anything) and names NDI when it is active ("NDI is on: 4 NDI
  senders running"). When NDI is active it offers **Pause NDI**, which does what the CTD menu item does. It shows at
  most once every 10 minutes while the condition lasts.

## Tests and CI

- **Unit tests:** `test/ctd-ndi`, a standalone CMake project like `test/ctd-update`, run in the **CTD OBS** workflow's
  Unit tests job, also under AddressSanitizer + UBSan and ThreadSanitizer. They cover:
  - the version allow-list
  - the loader paths: every produced path is in CTD's own folder or the bundle; every system path is forbidden; no env
    var or search path is used
  - SHA-256 against known vectors
  - the bounded queue: never above capacity, drops the oldest, buffers returned
  - the sender slots (the cap of 8, released on stop, never above the cap under 16 competing threads)
  - sender names, the run decision (each condition alone stops a sender) and the canvas size
  - the watchdog maths: rate, sustained growth, limit, re-arm, the 60 s log line
  - mode parsing, and safe mode / `--ctd-no-ndi` overriding it
- **Builds:** Windows x64 and both macOS jobs build `ctd-ndi`.
  - The macOS Package step requires `ctd-ndi.plugin` and fails if any `libndi*`, `*NDI*.plugin` or `*NDI*.driver` is
    inside the bundle.
  - The Windows job's Check NDI packaging step requires `ctd-ndi.dll` in the zip and fails on any `Processing.NDI.*`.
  - The Smoke test checks the module's `@rpath` libraries.
  - The signing script signs it with every other Mach-O.
- **Soak test** on a real Mac: `test/ctd-ndi/ctd-ndi-soak.sh`, steps below. CI can't do it: GitHub's Macs can't start
  CTD OBS, and there's no NDI network.

## Soak test (Mac, before merge)

What it proves: 60 minutes with 3 per-source NDI senders plus the program sender (4 senders), total memory growth
under 300 MB, the footprint logged every minute.

**Setup, once:**

1. The Mac must have **no NDI Tools** pieces. Install only the NDI 6 Runtime (`ndi.link/NDIRedistV6Apple`). The script
   checks for DAL/HAL NDI pieces and refuses to run if it finds any.
2. Install the CTD OBS build under test, open it.
3. In Settings ▸ CTD ▸ NDI:
   - choose **Send**
   - in the outputs list, tick **CTD OBS Program** and untick **CTD OBS Vertical**
   - press **Set up from NDI Runtime** if the library line doesn't say "NDI library: 6.4.0.2, CTD OBS's own verified
     copy"
   - OK

**Scene:**

1. In the program scene, have three sources that show a moving picture: e.g. a video capture device or a looping media
   file, a browser source with an animation, and a nested scene.
2. Add the **NDI® Output** filter to each of the three: right-click ▸ Filters ▸ + ▸ NDI® Output. Give them different
   names, e.g. "Soak 1", "Soak 2", "Soak 3".
3. Settings ▸ CTD ▸ NDI's outputs list shows four rows **Sending**.
4. Optional, to watch the senders: NDI Studio Monitor on **another** machine. Never install NDI Tools on this Mac.

**Run:**

1. In Terminal, from a checkout of this branch: `test/ctd-ndi/ctd-ndi-soak.sh`. (Or copy the one file anywhere and
   run it with `bash`.) It needs no sudo.
2. Leave the Mac alone for 60 minutes: no sleep (the script runs `caffeinate`), CTD OBS in front or not. Don't change
   the program scene: the per-source senders send only while their sources are on air.

**What the script does:**

- It finds CTD OBS's process and its current log, and waits for the watchdog line showing **4 senders**.
- Then it records, every 60 s for 60 minutes:
  - the watchdog's footprint (from inside CTD OBS: `phys_footprint`)
  - `ps` RSS as a cross-check
  - the NDI sender count and dropped frames

  These go to `~/Desktop/ctd-ndi-soak-<date>.csv`.
- **PASS:** the highest footprint minus the first is under **300 MB**, and every minute had 4 senders running.
- **FAIL:** anything else, including CTD OBS quitting. It prints the CSV path and a summary.

Send Juan's result (the summary lines and the CSV) to the SR with the PR. The SR merges only after a PASS.
