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
| Streaming | Service lists offer only CTD Relay and Custom RTMP; new profiles use a custom server | `frontend/settings/OBSBasicSettings_Stream.cpp`, `frontend/wizards/AutoConfigStreamPage.cpp`, `frontend/wizards/AutoConfig.cpp`, `frontend/widgets/OBSBasic_Service.cpp`, `frontend/data/locale/en-US.ini` |
| CTD plugin | Built in as a core module, from a submodule | `plugins/ctd-live-source`, `plugins/CMakeLists.txt`, `.gitmodules` |
| Build | Windows x64 CI | `.github/workflows/ctd-windows.yaml` |

## Not changed yet

- The program is still `obs64.exe`, and packages are still named `obs-studio-*`.
- Other languages and many English strings still say OBS.
- macOS and Linux paths and branding.
- The virtual camera registers the same Windows component as stock OBS, so it may conflict when both are installed.