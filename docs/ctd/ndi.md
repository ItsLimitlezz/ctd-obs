# NDI® in CTD OBS

NDI® is a registered trademark of Vizrt NDI AB. More about NDI: <https://ndi.video/>.

CTD OBS receives NDI sources and sends its program (and optionally the vertical canvas) as NDI, on macOS and Windows,
without putting anything system-wide on the machine and without being able to take a live show down with it.

**Why this design:** on 2026-11-22 Juan's NDI install (NDI Tools + Runtime + DistroAV, which also installed
CoreMediaIO DAL camera plugins and the `NDIAudio.driver` / `NDIAudioOut.driver` CoreAudio HAL drivers) made CTD OBS on
macOS leak about 1 GB a minute and crash during a live. Removing all of it fixed it (hub: `lessons/ctd-obs.md`). Every
rule below exists so that can't happen again.

## Rules

1. **No system pieces.** No DAL/CMIO camera plugins, no HAL audio drivers, no NDI Tools, no virtual camera or audio
   device, no DirectShow filters registered by CTD OBS. Only the NDI library, loaded into CTD OBS's own process, for
   receive and send.
2. **One exact, verified NDI library.** CTD OBS loads only its own copy of an NDI library whose SHA-256 is on the
   allow-list compiled into the build (`plugins/ctd-ndi/src/ndi-allowlist.cpp`). It never `dlopen`s / `LoadLibrary`s a
   file in `/usr/local/lib`, `/Library/NDI…`, Program Files, `PATH` or `NDI_RUNTIME_DIR_V6`, so another NDI install
   (or an update of it) can't change what runs inside CTD OBS. See [Where the library comes from](#where-the-library-comes-from).
3. **Off by default, a kill switch, a safe launch.** Settings ▸ CTD ▸ NDI: **Off** (default) / **Receive only** /
   **Receive + Send**. While Off, the NDI library isn't loaded at all. `--ctd-no-ndi` (and OBS's own `--safe-mode`)
   start CTD OBS with NDI off for that launch, whatever the setting; **CTD ▸ NDI ▸ Start without NDI…** restarts that
   way.
4. **Guardrails:** at most 8 NDI receivers at once, no frame queue that can grow, a memory watchdog, every NDI error
   caught and logged, a failed source shows a placeholder instead of crashing.

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
| No hooks into the window / preview | `preview-output.cpp` uses `obs_add_main_render_callback` with `gs_stage_texture` + `gs_stagesurface_map` in the same frame (a GPU stall on every frame); `main-output.cpp` and its Tools menu dialog are tied to the main window |
| No install prompts / network | ERR dialogs at startup; buttons that run `winget install NDI.NDIRuntime` / `brew reinstall libndi` in a shell; an update check POSTing the OBS install GUID and the plugin's hash to distroav.org |
| libndi not loaded until enabled | Loaded unconditionally in `obs_module_load` |
| Bounded queues, cap on receivers | No receiver cap. libobs's buffered async cache holds up to 30 frames per source (about 1 GB of 4K BGRA); the sender passes OBS's frame to `send_send_video_async_v2` without a copy (buffer lifetime race) |
| Built in-tree against our 33.0.1 headers | An obs-plugintemplate project (`buildspec.json` obs-studio 31.1.1, its own `project()`, `find_package(libobs)`); no in-tree build; needs Qt Network and libcurl |

Patching it meant a CTD fork of DistroAV changed in almost every file (the loader, plugin-main, config, both outputs,
forms, update, the source, CMake), kept in sync with upstream by hand. A small module of our own is less code to
review than that patch set, and it's easier to keep to these rules. DistroAV is not used, so it isn't in the About
credits. Its code was read for reference only; nothing is copied from it.

**`ctd-ndi`** is a core module: GPL-2.0-or-later, Windows and macOS, built in-tree in this repository's CI against
OBS 33.0.1 and signed with the app like the other modules. It needs no Qt, curl or frontend window code. Its parts:

| Part | What it does |
|---|---|
| `ndi-plugin.cpp` | Module load: registers the source and the output; reads the mode; loads NDI only when enabled; procs for the frontend; stops/starts the sends around profile and scene collection changes and on exit |
| `ndi-loader.cpp` | Set up and load, as above |
| `ndi-source.cpp` | **NDI® Source** (`ctd_ndi_source`), receive |
| `ndi-output.cpp` | **NDI send** (`ctd_ndi_output`): program ("CTD OBS Program") and vertical ("CTD OBS Vertical") |
| `ndi-finder.cpp` | One background finder thread (`NDIlib_find_wait_for_sources`), started the first time a source's properties open, stopped when NDI goes off |
| `ndi-logic.cpp/.hpp` | Mode parsing, allow-list matching, path policy, SHA-256, the bounded queue, the receiver slots: no OBS, no NDI, unit-tested |

Nothing in it touches the main window, the preview, a render callback, a display or a Qt widget. Its only frontend use
is `obs_frontend_get_user_config` and frontend events.

### Receive (NDI® Source)

- **Properties:**
  - NDI source: a list of the sources found, editable.
  - Bandwidth: Highest / Lowest.
  - Audio on/off.
  - A status line, and the ndi.video link.
- **Threading:** one worker thread per source.
  - It holds a receiver slot (at most **8** receivers in CTD OBS).
  - `NDIlib_recv_create_v3` with `color_format = UYVY_BGRA`, `allow_video_fields = false`.
  - It calls `NDIlib_recv_capture_v3(…, 100 ms)` in a loop and hands each frame straight to libobs, then frees it with
    `NDIlib_recv_free_*` on the same iteration. Every path frees what it captured; NDI's own queue is drained all the
    time, also while the source is hidden.
- **Queue bound:**
  - The source is **unbuffered** (`obs_source_set_async_unbuffered`): libobs keeps only the newest frame, never a
    queue. That bound is 1 frame per source; the worst case is 8 sources × one 4K BGRA frame ≈ 270 MB.
  - CTD's code holds no frames.
  - The log warns if NDI's own receive queue (`NDIlib_recv_get_queue`) ever holds more than 4 video frames.
- **Placeholder** (a dark 16:9 frame) when:
  - NDI is off or paused
  - the 8-receiver cap is reached (it retries every 2 s)
  - creating the receiver fails
  - NDI reports an error
  - the sender has gone (no connection for 3 s)
  - a frame comes in a format the source doesn't handle

  The reason goes to the log once per change and to the source's status line.
- **Errors:** NDI is a C API; every call's result is checked. The worker body also catches any C++ exception, logs
  it, shows the placeholder and backs off.
- **Scene data:** the source type is registered even when NDI is off, so scenes with NDI sources load, keep their
  settings and save unchanged. They just show the placeholder.

### Send

- **The output:** `ctd_ndi_output` is an `OBS_OUTPUT_AV` raw output (no encoder, no render callback).
  - "CTD OBS Program" sends the main video and audio.
  - "CTD OBS Vertical" sends the canvas named `Vertical` (`obs_get_canvas_by_name`) with the main audio.
- **When it runs:** only in Receive + Send mode, from when loading finishes until exit, mode change or pause. Each send
  can be switched on or off in Settings.
- **Video:**
  - `raw_video` runs on OBS's video thread. It copies the frame into one of **3 preallocated buffers** and pushes it
    into a **bounded queue of 2**: when the queue is full, the oldest frame is dropped and its buffer reused.
  - A sender thread pops frames and calls `NDIlib_send_send_video_v2` (synchronous, so NDI never reads a buffer OBS
    or we reuse).
  - Memory is fixed at 3 frames per send, whatever NDI or the network does. Dropped frames are counted and logged
    every 60 s.
  - NV12, I420, UYVY, BGRA, BGRX and RGBA go as they are. Other formats are converted to NV12 by OBS
    (`obs_output_set_video_conversion`).
  - The frame rate is the exact `fps_num/fps_den`.
- **Audio:** planar float, copied into one reusable buffer and sent with `NDIlib_send_send_audio_v3`.
- **Switches and updates:** the sends stop on `PROFILE_CHANGING` / `SCENE_COLLECTION_CHANGING` and start again after
  the change.
  - `OBSBasic::CtdAnyOutputActive` doesn't count `ctd_ndi_output`, so NDI never blocks a profile/collection switch or
    an update install.
  - Like any active output, it does hold the vertical canvas's resolution: stop the vertical send before changing the
    vertical size.

### Mode and kill switch

| Mode (`[CTD] NDIMode` in user.ini) | NDI library | Receivers | Sends |
|---|---|---|---|
| `off` (default; also anything unknown) | not loaded | placeholder | none |
| `receive` | loaded | yes | none |
| `receive_send` | loaded | yes | program (`[CTD] NDISendProgram`, default on), vertical (`[CTD] NDISendVertical`, default off) |

- **Changing the mode** (Settings ▸ CTD ▸ NDI, OK/Apply) takes effect at once:
  - Going **to Off** stops every receiver thread (receivers destroyed), the finder and the sends, within about 100 ms.
  - The library stays mapped until CTD OBS quits, because unloading it while NDI's own threads wind down isn't safe.
    Settings says "NDI is off. The library stays in memory, unused, until CTD OBS restarts."
  - Going **on** loads it if it isn't loaded yet.
- **Launching without NDI:** `--ctd-no-ndi` or OBS's `--safe-mode` starts with NDI off for that launch, whatever the
  setting; Settings shows "Off for this launch". **CTD ▸ NDI ▸ Start without NDI…** asks, then restarts CTD OBS with
  `--ctd-no-ndi` (it warns first when streaming or recording, since a restart ends them).
- **CTD ▸ NDI ▸ Pause NDI** stops all receivers and sends at once, without restarting and without changing the
  setting. **Resume NDI** starts them again.

### Memory watchdog

The watchdog is in the CTD OBS app, so it watches everything, not only NDI: `frontend/ctd/ctd-memory-watchdog.cpp`,
with the maths in `ctd-memory-watchdog-logic.cpp` (unit-tested).

- **What it measures** (every 10 s):
  - macOS: `task_info(TASK_VM_INFO).phys_footprint`, the number Activity Monitor shows. It includes IOSurfaces and
    graphics memory owned by the process.
  - Windows: private bytes (`PROCESS_MEMORY_COUNTERS_EX.PrivateUsage`) plus the process's local video memory on the
    main adapter (`IDXGIAdapter3::QueryVideoMemoryInfo(...).CurrentUsage`).
- **Log, every 60 s:**
  `[ctd-watchdog] footprint 1834 MB (+12 MB/min over 3 min), NDI receive_send: 2 receivers, 1 send, 0 dropped`.
- **Warning when either holds:**
  - It grew by at least **200 MB in each of the last 3 one-minute intervals** (sustained, so one spike such as a scene
    load doesn't count).
  - It passed **6144 MB** (`[CTD] WatchdogLimitMB` in user.ini).

  The warning is non-blocking (a window that doesn't stop anything) and names NDI when it is active ("NDI is on: 2
  receivers, 1 send"). When NDI is active it offers **Pause NDI**, which does what the CTD menu item does. It shows at
  most once every 10 minutes while the condition lasts.

## Tests and CI

- **Unit tests:** `test/ctd-ndi`, a standalone CMake project like `test/ctd-update`, run in the **CTD OBS** workflow's
  Unit tests job, also under AddressSanitizer + UBSan and ThreadSanitizer. They cover:
  - the version allow-list
  - the loader paths: every produced path is in CTD's own folder or the bundle; every system path is forbidden; no env
    var or search path is used
  - SHA-256 against known vectors
  - the bounded queue: never above capacity, drops the oldest, buffers returned
  - the receiver slots (the cap of 8, released on destroy)
  - the watchdog maths: rate, sustained growth, limit, re-arm, the 60 s log line
  - mode parsing, and safe mode / `--ctd-no-ndi` overriding it
- **Builds:** Windows x64 and both macOS jobs build `ctd-ndi`.
  - The macOS Package step requires `ctd-ndi.plugin` and fails if any `libndi*`, `*NDI*.plugin` or `*NDI*.driver` is
    inside the bundle.
  - The Windows job's Check NDI packaging step requires `ctd-ndi.dll` in the zip and fails on any `Processing.NDI.*`.
  - The Smoke test checks the module's `@rpath` libraries.
  - The signing script signs it with every other Mach-O.
- **Soak test** on a real Mac: `test/ctd-ndi/ctd-ndi-soak.sh`, steps below. CI can't do it: GitHub's Macs can't start CTD OBS,
  and there's no NDI network.

## Soak test (Mac, before merge)

What it proves: 60 minutes with 2 NDI receivers and 1 NDI send, total memory growth under 300 MB.

**Setup, once:**

1. The Mac must have **no NDI Tools** pieces. Install only the NDI 6 Runtime (`ndi.link/NDIRedistV6Apple`). The script
   checks for DAL/HAL NDI pieces and refuses to run if it finds any.
2. Install the CTD OBS build under test, open it.
3. In Settings ▸ CTD ▸ NDI:
   - choose **Receive + Send**
   - keep **Send the program** on and **Send the vertical canvas** off (the test counts exactly 1 send)
   - press **Set up from NDI Runtime** if the library line doesn't say "NDI library: 6.4.0.2, CTD OBS's own verified
     copy"
   - OK

**Scene:**

1. Make a scene with **two NDI® Sources**:
   - Source 1: **another machine's** NDI source (e.g. the show PC's CTD OBS "CTD OBS Program", or any NDI camera).
   - Source 2: this Mac's own "CTD OBS Program" (a loopback, so it also exercises the send).
2. Make it the program scene. Both must show video.

**Run:**

1. In Terminal, from a checkout of this branch: `test/ctd-ndi/ctd-ndi-soak.sh`. (Or copy the one file anywhere and
   run it with `bash`.) It needs no sudo.
2. Leave the Mac alone for 60 minutes: no sleep (the script runs `caffeinate`), CTD OBS in front or not.

**What the script does:**

- It finds CTD OBS's process and its current log, and waits for the watchdog line showing **2 receivers, 1 send**.
- Then it records, every 60 s for 60 minutes:
  - the watchdog's footprint (from inside CTD OBS: `phys_footprint`)
  - `ps` RSS as a cross-check
  - the NDI counts

  These go to `~/Desktop/ctd-ndi-soak-<date>.csv`.
- **PASS:** the highest footprint minus the first is under **300 MB**, and every minute had 2 receivers and 1 send.
- **FAIL:** anything else, including CTD OBS quitting. It prints the CSV path and a summary.

Send Juan's result (the summary lines and the CSV) to the SR with the PR. The SR merges only after a PASS.
