# CTD OBS changes from OBS Studio

CTD OBS is based on [OBS Studio](https://github.com/obsproject/obs-studio) and stays under the GPL. Every change to
OBS's own behaviour is listed here, so merging a new OBS release stays manageable.

Base: OBS Studio 33.0.1.

| Area | What changed | Where |
|---|---|---|
| Config | `ctd-obs` folder instead of `obs-studio` for settings, scenes, logs, plugins and the shader cache | `frontend/` paths, `libobs-d3d11/d3d11-subsystem.cpp`, `libobs-d3d11/d3d11-shader.cpp` |
| Running beside OBS | Its own single-instance check | `frontend/utility/platform-windows.cpp` |
| Updates | Updater off, update menu items hidden | `frontend/obs-main.cpp`, `frontend/widgets/OBSBasic.cpp` |
| OBS's servers | No What's New, service list, game-capture data or wizard platform ingest downloads | `frontend/cmake/feature-whatsnew.cmake`, `plugins/rtmp-services/CMakeLists.txt`, `plugins/win-capture/plugin-main.c` |
| Reports | No crash report offer; log upload, help, website and Discord menu items hidden | `frontend/OBSApp.cpp`, `frontend/widgets/OBSBasic.cpp` |
| Name | Window title, Windows file details, About text (English) | `frontend/widgets/OBSBasic.cpp`, `frontend/cmake/windows/obs.rc.in`, `frontend/data/locale/en-US.ini` |
| Look | CTD Night theme as the default, Space Mono font, CTD icons | `frontend/data/themes/CTD_Night.ovt`, `frontend/cmake/templates/ui-config.h.in`, `frontend/obs-main.cpp`, `frontend/forms/obs.qrc`, `frontend/forms/fonts/`, `frontend/forms/images/`, `frontend/cmake/windows/obs-studio.ico` |
| Streaming | Service lists offer only CTD Relay and Custom RTMP; new profiles use a custom server; simple and advanced output modes stream a keyframe every 2 s (advanced only when the user has not set one; a separate recording encoder is left alone) | `frontend/settings/OBSBasicSettings_Stream.cpp`, `frontend/wizards/AutoConfigStreamPage.cpp`, `frontend/wizards/AutoConfig.cpp`, `frontend/widgets/OBSBasic_Service.cpp`, `frontend/utility/SimpleOutput.cpp`, `frontend/data/locale/en-US.ini` |
| CTD plugin | Built in as a core module, from a submodule | `plugins/ctd-live-source`, `plugins/CMakeLists.txt`, `.gitmodules` |
| Vertical canvas | Built in as the core module `vertical-canvas`, from a submodule of the CTD fork of Aitum Vertical (branch `ctd-obs-in-tree`); no version check against Aitum's server | `plugins/ctd-vertical-canvas`, `plugins/CMakeLists.txt`, `.gitmodules` |
| Program name | `ctd-obs64.exe` instead of `obs64.exe` (updater, debugger settings, virtual camera self-check and file details follow); Windows packages are `ctd-obs-*-windows-x64.zip` | `frontend/CMakeLists.txt`, `frontend/cmake/windows/obs.rc.in`, `frontend/updater/updater.cpp`, `cmake/windows/cpackconfig.cmake`, `cmake/bundle/windows/ALL_BUILD.vcxproj.user.in`, `plugins/win-dshow/virtualcam-module/virtualcam-filter.cpp`, `.github/scripts/Package-Windows.ps1`, `.github/workflows/ctd-windows.yaml` |
| Virtual camera | Own CLSID (`F65F7748-B099-4687-8DE4-834E7A944F0B`), device name "CTD OBS Virtual Camera", DLLs `ctd-obs-virtualcam-module32/64.dll`, shared-memory name `CTDOBSVirtualCamVideo` and resolution file `%APPDATA%\ctd-obs-virtualcam.txt`, so it can be installed and run beside stock OBS's virtual camera | `CMakePresets.json`, `cmake/windows/helpers.cmake`, `plugins/win-dshow/virtualcam-module/`, `plugins/win-dshow/virtualcam.c`, `shared/obs-shared-memory-queue/shared-memory-queue.c` |
| Strings | English UI says CTD OBS where it means the app (credits, licence and obs-websocket untouched) | `frontend/data/locale/en-US.ini`, `frontend/obs-main.cpp`, `frontend/widgets/OBSBasic_SysTray.cpp`, `frontend/oauth/AuthListener.cpp`, `frontend/plugin-manager/PluginManager.cpp` |
| Show Profiles | `ctd_show_apply` / `ctd_show_refresh` procs (C1): load a scene collection + profile from staged files without saving the active one over them first; no restart prompt (reported as `restart_required`); obs-websocket vendor `ctd-obs` for tests. Spec: `docs/ctd/show-profiles-core.md` | new `frontend/ctd/ctd-show-api.cpp`, `frontend/cmake/ctd-show.cmake`, `frontend/CMakeLists.txt`, `frontend/widgets/OBSBasic.cpp` (registration), `frontend/widgets/OBSBasic.hpp`, `frontend/widgets/OBSBasic_Profiles.cpp` (`ActivateProfile` optional argument) |
| Show Profiles | Scene collection and profile switching blocked while any output is active (C2): menus, frontend API and obs-websocket; "Stop streaming and recording to switch." | `frontend/ctd/ctd-show-api.cpp`, `frontend/widgets/OBSBasic_SceneCollections.cpp`, `frontend/widgets/OBSBasic_Profiles.cpp`, `frontend/data/locale/en-US.ini` |
| Show Profiles | `ctd_show_refresh` keeps the active scene collection listed (and its in-memory state) when the rescan doesn't find its file, so a fresh config's unsaved "Untitled" survives the plugin's startup probe and gets saved | `frontend/ctd/ctd-show-api.cpp` |
| Build | Windows x64 CI | `.github/workflows/ctd-windows.yaml` |
| Installer | NSIS setup `CTD-OBS-Setup-<version>-x64.exe` built next to the zip: installs per machine to `C:\Program Files\CTD OBS`, Start menu and optional desktop shortcut, registers the CTD OBS virtual camera, "CTD OBS" Add/Remove Programs entry (publisher CineTech Digital), VC++ runtime when missing, own registry keys only (side by side with OBS Studio); uninstall keeps `%APPDATA%\ctd-obs` unless "Remove settings" is ticked; code signing hook left for later | `cmake/windows/installer/ctd-obs.nsi`, `.github/scripts/Package-Windows.ps1`, `.github/workflows/ctd-windows.yaml` |

## Not changed yet

- Other languages still say OBS.
- macOS and Linux paths and branding.
