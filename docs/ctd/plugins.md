# Third-party plugins in CTD OBS

CTD OBS loads third-party plugins **only from its own plugin folder**, never from OBS Studio's. A plugin installed for
your regular OBS Studio is built against that OBS's libobs and Qt and can crash CTD OBS, and installing something for
CTD OBS must never change your OBS Studio setup. CTD OBS's built-in modules (OBS's own plugins, the CTD plugin, the
vertical canvas) are inside the app and aren't affected.

Code: `frontend/plugin-manager/PluginModuleLoader_*` (where plugins load from), `frontend/ctd/ctd-plugins.cpp` (the list
of OBS Studio's plugins), `frontend/ctd/ctd-plugin-isolation-logic.cpp` (finding and copying them; unit tests in
`test/ctd-plugins`).

## Where CTD OBS loads plugins from

| Platform | CTD OBS's folder | Never loaded (OBS Studio's) |
|---|---|---|
| macOS | `~/Library/Application Support/ctd-obs/plugins/<name>.plugin` | `~/Library/Application Support/obs-studio/plugins`, `/Library/Application Support/obs-studio/plugins` |
| Windows | `C:\ProgramData\ctd-obs\plugins\<name>\bin\64bit\<name>.dll` (+ `data\`) | `C:\ProgramData\obs-studio\plugins`, `C:\Program Files\obs-studio\obs-plugins\64bit` |
| Linux | `$XDG_DATA_HOME/ctd-obs/plugins/<name>/` (default `~/.local/share`), legacy `$XDG_CONFIG_HOME/ctd-obs/plugins/<name>/bin/64bit/` | `…/obs-studio/plugins` in both; `/usr/lib/…/obs-plugins` and `…/obs-modules/plugins` when CTD OBS is installed to `/usr` or `/usr/local` |

The environment variables are CTD OBS's own: `CTD_OBS_PLUGINS_PATH`, `CTD_OBS_LEGACY_PLUGINS_PATH`,
`CTD_OBS_LEGACY_PLUGINS_DATA_PATH`. OBS Studio's `OBS_PLUGINS_PATH` and friends are ignored.

Each loaded or skipped plugin is in the log as usual. At launch CTD OBS also logs each plugin it finds in OBS Studio's
folders: `[CTD] plugins: 'distroav' is installed for OBS Studio (…), not loaded`.

## Plugins from OBS Studio

The first time CTD OBS finds plugins in OBS Studio's folders, it lists them once: **"These belong to your regular OBS and
aren't loaded in CTD OBS."** **CTD ▸ Advanced ▸ Plugins from OBS Studio…** opens the same list at any time. It isn't
shown for automated launches (`--startstreaming`, `--minimize-to-tray`, …) or while an output is active. Modules that
CTD OBS has built in (for example an old standalone CTD plugin or Aitum Vertical install) aren't listed.

Each plugin has **Copy into CTD OBS**:

- It asks first and warns that the plugin **must be built for OBS 33**, because CTD OBS is based on OBS Studio 33.0.1. A
  plugin built for another OBS version can crash CTD OBS.
- It copies the plugin into CTD OBS's folder. It copies into a temporary name next to the destination and then renames
  it, so there is never a half-copied plugin. A plugin from OBS Studio's Windows install folder is copied in the legacy
  layout `<name>\bin\64bit\<name>.dll` + `<name>\data`.
- **OBS Studio's files are only read: never moved, changed or deleted.** If a plugin with that name is already in
  CTD OBS's folder, nothing is copied.
- Restart CTD OBS to load it. **CTD ▸ Advanced ▸ Plugin Manager** (also **Tools ▸ Plugin Manager**) turns it off again:
  untick it, restart.

You can also install a plugin for CTD OBS by hand: put it in CTD OBS's folder (table above). **Open CTD OBS's Plugin
Folder** in the list opens that folder.

## NDI (DistroAV)

CTD OBS does **not** bundle NDI. To use NDI, install [DistroAV](https://github.com/DistroAV/DistroAV) (GPL-2.0) and the
NDI Runtime, then copy DistroAV into CTD OBS.

Versions for CTD OBS (OBS 33.0.x):

| | macOS (Apple Silicon) | Windows x64 |
|---|---|---|
| DistroAV | **6.2.2** (`distroav-6.2.2-macos-universal.pkg`) | **6.2.2** (`distroav-6.2.2-windows-x64` installer or zip) |
| NDI Runtime | **NDI 6 Runtime for Apple, 6.3 or later** (installs `/usr/local/lib/libndi.dylib`) | **NDI 6 Runtime, 6.3 or later** |

- DistroAV 6.2.x needs OBS 31.1.1 or later and NDI 6.3.0 or later. 6.2.2 (October 2026) is the first release built
  for OBS 33's plugin layout.
- 6.1.x and older need older NDI runtimes, and 5.x needs NDI 5. Don't use them with CTD OBS.
- If the runtime is missing, too old or can't start, DistroAV logs an error (`ERR-401`…`ERR-406`, `ERR-425`), shows a
  dialog and stays off (no NDI sources or outputs). It doesn't crash. Install or update the runtime and restart.

**Plugin isolation doesn't cover NDI's system components.** On macOS, the NDI Runtime and NDI Tools installers
also put system-wide parts in place:
- an audio driver in `/Library/Audio/Plug-Ins/HAL`;
- a camera plugin, `NDI Virtual Input`, in `/Library/CoreMediaIO/Plug-Ins/DAL`;
- apps, such as Access Manager and Monitor.

macOS loads these into **every** app that lists audio devices or cameras, including CTD OBS. That happens whatever
CTD OBS's plugin folders contain. On 2026-11-22 they caused CTD OBS on macOS to freeze and crash about every 15
minutes, with GPU memory growing to about 17 GB. Removing NDI's audio driver, DAL plugins and apps, plus DistroAV,
and rebooting fixed it. Install only the **NDI Runtime** that DistroAV needs, not the full NDI Tools. If CTD OBS
freezes or its memory grows after an NDI install, remove those components and reboot first.

Steps:

1. Install the NDI Runtime (above). On macOS, the NDI 6 Runtime for Apple installer; on Windows the NDI 6 Runtime
   installer (or `winget install NDI.NDIRuntime`).
2. Install DistroAV 6.2.2 for OBS Studio as usual. Its installers only install for OBS Studio: macOS into
   `~/Library/Application Support/obs-studio/plugins/distroav.plugin`, Windows into
   `C:\ProgramData\obs-studio\plugins\distroav`.
3. Open CTD OBS → **CTD ▸ Advanced ▸ Plugins from OBS Studio…** → **Copy into CTD OBS** next to **distroav**. Then
   restart CTD OBS.
4. macOS 15 and later: allow **Local Network** for **CTD OBS** when asked (System Settings ▸ Privacy & Security ▸ Local
   Network). CTD OBS has its own permissions, separate from OBS Studio's. Without it, NDI sources don't show up.
5. If CTD OBS and OBS Studio run at the same time, give CTD OBS's NDI output (**Tools ▸ DistroAV NDI Settings**) a
   different name from OBS Studio's. Two senders with the same name make DistroAV's output fail to start (older
   versions crashed on exit because of it).

To turn NDI off again: **CTD ▸ Advanced ▸ Plugin Manager** → untick **DistroAV** → restart. You can also delete
`distroav.plugin` / `distroav` from CTD OBS's folder; OBS Studio's copy stays.

### Why not built in

We looked at building DistroAV into CTD OBS's CI, as with the vertical canvas, and decided against it for now:

- It has no in-tree build: we would have to keep a fork.
- Its update check (when the user turns it on) sends OBS's install ID, versions and a hash of the plugin to
  distroav.org. We would have to remove that.
- Shipping NDI features adds NDI's licence obligations (an ndi.video link next to the NDI features, NDI's trademark
  line in About).
- We would become the support contact for NDI crashes.

DistroAV 6.2.2 already runs on OBS 33, so a CTD build would not be more compatible. We'll revisit this if a crash report
shows an ABI problem with the released build.

DistroAV's own known issues worth checking when NDI misbehaves:

- [#1215](https://github.com/DistroAV/DistroAV/issues/1215) (Apple Silicon): a crash in NDI's video thread after about
  13 minutes when an old Homebrew FFmpeg (before 7) was installed. Upgrade it.
- [#1335](https://github.com/DistroAV/DistroAV/issues/1335): a crash on exit when the NDI output couldn't start (fixed
  in 6.2.0).
