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

## NDI

CTD OBS does **not** bundle NDI, and **DistroAV is not the supported route**. NDI is built into CTD OBS as the CTD
NDI output (the core module `ctd-ndi`; spec: [ndi.md](./ndi.md)).

- **What it sends:** the program, the vertical canvas, or any source, scene or group, each as its own NDI sender (the
  **NDI® Output** filter). CTD OBS only sends NDI; it doesn't receive it.
- **Turning it on:** Settings ▸ CTD ▸ NDI ▸ **Send**. It's off by default.
- **What it needs:** the official **NDI 6 Runtime**, once, never NDI Tools. CTD OBS checks the Runtime's library
  against its allow-list and loads only its own verified copy.

**Plugin isolation doesn't cover NDI's system components.** On macOS, the NDI Tools installer (not the plain NDI
Runtime) puts system-wide parts in place:

- an audio driver in `/Library/Audio/Plug-Ins/HAL`
- a camera plugin, `NDI Virtual Input`, in `/Library/CoreMediaIO/Plug-Ins/DAL`
- apps, such as Access Manager and Monitor

macOS loads these into **every** app that lists audio devices or cameras, including CTD OBS. That happens whatever
CTD OBS's plugin folders contain. On 2026-11-22 they caused CTD OBS on macOS to freeze and crash about every 15
minutes, with GPU memory growing to about 17 GB. Removing NDI's audio driver, DAL plugins and apps, plus DistroAV,
and rebooting fixed it.

Install only the **NDI 6 Runtime**, not NDI Tools. Settings ▸ CTD ▸ NDI warns when NDI Tools' pieces are installed.
If CTD OBS freezes or its memory grows after an NDI install, remove those components and reboot first.

**DistroAV:** don't copy it into CTD OBS with **Plugins from OBS Studio…**.

- It loads NDI from system folders.
- Its preview output and NDI filter stall the GPU on every frame.
- It has an update check that sends OBS's install ID to distroav.org.

[ndi.md](./ndi.md) has the full comparison. If an older setup has it in CTD OBS's plugin folder, turn it off in
**CTD ▸ Advanced ▸ Plugin Manager** (untick **DistroAV**, restart) or delete `distroav.plugin` / `distroav` from CTD
OBS's folder; OBS Studio's copy stays.
