# Show Profiles, Spike S1: applying staged files and the vertical canvas

Branch `ctd/spike-s1`, from `ctd/main` (OBS Studio 33.0.1 base). Throwaway branch: the prototype is here to prove the approach compiles and works, not to be merged as it is.

Vertical canvas source read: `ItsLimitlezz/ctd-vertical-canvas` @ `047ef96` (Aitum Vertical).

The hub design (`epics/show-profiles-design.md`) couldn't be read from this session, because the repository wasn't attached. This spike works from ticket S1.

## Findings

### Scene collections (`frontend/widgets/OBSBasic_SceneCollections.cpp`)

1. **Re-applying the active collection can't be done with stock code.** `ChangeSceneCollection()` returns early when the selected name equals the current one. `obs_frontend_set_current_scene_collection()` only triggers that same menu action, so it does nothing for the active collection either.
2. **Save-on-switch.** `ActivateSceneCollection()` calls `SaveProjectNow()` only when the target name differs from the current one. That save writes the *outgoing* collection to *its own* file, which is the right behaviour for a normal switch.
3. **The real risk for a re-apply is a normal save between the write and the load.** About 25 places call `SaveProject()`, which queues `SaveProjectDeferred()`. They include scene and source edits, filters, transitions, studio mode, outputs, and every settings OK. If a caller writes the staged JSON over the active collection's file and then asks OBS to load it, any queued or new save in between writes the in-memory scenes over the staged file. All saves are gated by `disableSaving` (a counter) and `projectChanged`.
4. `Load()` reads with `obs_data_create_from_json_file_safe(path, "bak")`. If the file is unreadable, it is renamed to `.json.invalid` and a default scene is created. `LoadData()` raises the frontend events `SCENE_COLLECTION_CLEANUP` (from `ClearSceneData`) and then `SCENE_COLLECTION_CHANGED` (from `ActivateSceneCollection`).
5. The collection cache (`RefreshSceneCollectionCache`) is rebuilt from `basic/scenes/*.json`, and each entry is keyed by the file's `"name"`. A file dropped into that folder shows up after a cache refresh, with no restart.
6. `LoadData()` can open the modal *Missing Files* dialog. If a staged collection points at files that aren't on the PC, apply blocks until the operator closes it. Show Profiles must ship media with the show, or we suppress the check during apply (follow-up).

### Profiles (`frontend/widgets/OBSBasic_Profiles.cpp`)

1. `ChangeProfile()` also returns early for the active profile.
2. `ActivateProfile(profile, reset)` opens the new `basic.ini` **first**, then saves the old in-memory `activeConfiguration` to the old profile's file, then swaps. For a re-apply of the active profile, the old config does briefly overwrite the staged `basic.ini`, but the new config is already in memory and is saved back at the end. The net result is correct. It depends on that order, so any change to `ActivateProfile` must keep it.
3. With `reset = true` it calls `ResetVideo()` (runtime `obs_reset_video`, no restart), `InitService()` (reads `service.json`), `ResetOutputs()` (reads `streamEncoder.json` and `recordEncoder.json`) and rebuilds hotkeys.
4. **The restart prompt** is a frontend `QMessageBox` at the end of `ActivateProfile`. It appears only when `GetRestartRequirements()` finds a different audio sample rate or speaker layout. Nothing else in a profile needs a restart.
5. Profile files: `basic.ini` (required; `[General] Name` is the profile name), plus optional `service.json`, `streamEncoder.json` and `recordEncoder.json`. These are the same files as stock import and export.

### Output state

- Stock OBS disables the whole **Profile** menu while a *frontend* output is active (`OnActivate`/`OnDeactivate` in `OBSBasic.hpp`). The **Scene Collection** menu is never disabled.
- Plugin outputs (the vertical canvas's stream, record and backtrack outputs) raise no frontend events, so stock OBS doesn't see them at all. The only reliable check is `obs_enum_outputs` + `obs_output_active`.

### Canvases in libobs (`libobs/obs-canvas.c`, `libobs/obs.c`)

- `obs_save_canvas()` stores only `name`, `uuid`, `private` and `flags`. It stores no video settings and no scenes.
- Scenes belong to a canvas through `"canvas_uuid"`, which is written per source by `obs_save_source` (`obs.c:2506`) and resolved on load (`obs.c:2285`).
- The frontend saves the canvases that plugins register with `obs_frontend_add_canvas` in the collection JSON's `"canvases"` array, and saves their scenes in `"sources"`. Ephemeral canvases are skipped.

### Vertical canvas (Aitum Vertical, our fork)

The plugin stores its data in two places:

| What | Where | Scope |
|---|---|---|
| Vertical **scenes** and their sources, the canvas name and uuid (`"Aitum Vertical"`), and per-scene `order` and `canvas_active` (written by its `save_load` callback) | The **scene collection JSON** (`"canvases"` + `"sources"` with `canvas_uuid`) | Per collection, so it already travels with a staged collection |
| The **dock config**: resolution, bitrates, encoders and encoder settings, record path and format, backtrack, delay, virtual camera mode, hotkeys, transitions, the current vertical scene, and **`stream_outputs[]`** (`name`, `enabled`, `stream_server`, `stream_key`, plus per-output encoder settings) | `plugin_config/vertical-canvas/config.json`, key `"canvas"` (an array with one object per dock) | **Global**: the same for every collection and profile. Stream keys are stored in plain text |

- **Save times for `config.json`:** `OBS_FRONTEND_EVENT_EXIT`, closing the config dialog, and adding a dock. It is **not** saved on a collection or profile change.
- **Load:** once only, in `obs_module_post_load` (each dock is built from its settings object). There is no runtime reload.
- **Runtime resolution change:** already exists in the config dialog (`config-dialog.cpp:1257`). It does `obs_canvas_reset_video`, then `LoadScenes()`, then `ProfileChanged()`.
- **Existing hooks:** procs `aitum_vertical_get_stream_settings` and `aitum_vertical_set_stream_settings` (the whole `stream_outputs` array), and obs-websocket vendor `aitum-vertical-canvas` requests `update_stream_server` and `update_stream_key`. Those vendor requests only change the in-memory string and don't persist it.
- **Scene collection switches:** the plugin handles them already. `CLEANUP` calls `ClearScenes()` and `CHANGED` calls `LoadScenes()`, so `ctd_show_apply` reloads vertical scenes with no extra work.

### obs-websocket

obs-websocket 5 can't call global procs. For testing, the prototype registers an obs-websocket vendor `ctd-obs` (requests `show_apply` and `show_refresh`) that forwards to the procs. It uses the same proc calls as `obs-websocket-api.h`, so the frontend doesn't need the obs-websocket headers.

## C1: `ctd_show_apply` and `ctd_show_refresh` (final)

```
void ctd_show_apply(in string collection, in string profile,
                    out bool success, out string error, out bool restart_required)
void ctd_show_refresh(out bool success, out string error)
```

Both are registered on `obs_get_proc_handler()` in `OBSBasic::OnFirstLoad()`, just before `OBS_FRONTEND_EVENT_FINISHED_LOADING`, so they exist before any plugin's finished-loading handler runs. Both can be called from any thread. They run on the UI thread through `QMetaObject::invokeMethod(main, …, WaitConnection())`, which blocks when called from another thread and runs directly when called from the UI thread.

**`ctd_show_apply`**

- `collection` is the absolute path to a **staged** scene collection JSON. It must be **outside** `basic/scenes/` (for example, the Show Profiles staging folder), and its `"name"` is the target collection. An empty string means the collection is left as it is.
- `profile` is the absolute path to a **staged** profile folder. It must contain `basic.ini`, and its `[General] Name` (or, failing that, the folder name) is the target profile. An empty string means the profile is left as it is.
- `success` is false, with `error` set, in these cases: `"nothing to apply"`, `"busy: a scene collection is loading"` (when `disableSaving > 0` or a previous clear failed), `"outputs active"`, an invalid file, or a file or folder name collision.
- `restart_required` is true when the applied profile changes the audio sample rate or speaker layout. OBS keeps running with the old audio until it restarts. No prompt is shown.

Order of operations (UI thread):

1. Run the guards: something to apply, not loading, **no output active** (any output, through `obs_enum_outputs`).
2. Validate the staged files: parse the JSON, read `"name"`, and open the staged `basic.ini`. Nothing has been changed at this point.
3. Resolve the targets. An existing collection or profile with that name means its file or folder is the target. Otherwise the target is a new file or folder named after the staged one, and the call fails if that name is already in use.
4. If the target collection **is not** the active one, run `SaveProjectNow()`. This is stock behaviour that keeps the operator's unsaved edits to the outgoing collection.
   If it **is** the active one, skip this step. **This is the skipped save-on-switch.**
5. Run `disableSaving++` and `projectChanged = false`. From here until the new collection is loaded, no save can run, whether it is queued, deferred or new.
6. **Apply the profile first**, so video settings are in place before the collection loads:
   - Copy `basic.ini`, `service.json`, `streamEncoder.json` and `recordEncoder.json` into the target folder. Delete any optional file that the staged folder doesn't have.
   - Run `RefreshProfileCache()`, then `PROFILE_CHANGING`, then `ActivateProfile(profile, true, &requirements)`. The new third argument makes it return the requirements instead of showing the prompt.
7. **Apply the collection:**
   - Copy the staged JSON over the target file.
   - Run `RefreshSceneCollectionCache()`, then `SCENE_COLLECTION_CHANGING`, then `ActivateSceneCollection()`. Its own `SaveProjectNow()` is a no-op because `disableSaving > 0`, and it then runs `Load()`, `SCENE_COLLECTION_LIST_CHANGED` and `SCENE_COLLECTION_CHANGED`.
8. Run `projectChanged = false` and `disableSaving--`. Return `!clearingFailed`.

Hook points in stock code: `ActivateProfile` gets the optional `std::vector<std::string> *ctdRestartRequirements` argument. All the other changes are new code in `frontend/widgets/OBSBasic_CtdShow.cpp`. `ChangeSceneCollection`, `ActivateSceneCollection` and `Load` are otherwise unchanged.

**`ctd_show_refresh`**

- Rescans `basic/scenes` and `basic/profiles` (`RefreshSceneCollections(true)` and `RefreshProfiles(true)`), rebuilds both menus, and raises `SCENE_COLLECTION_LIST_CHANGED` and `PROFILE_LIST_CHANGED`. It loads nothing and saves nothing.
- Use it when Show Profiles adds or removes collection or profile files that aren't being applied right now, so they show up in the menus without a restart.
- It fails with `"busy…"` while a load is in progress.

Rules for callers (the Show Profiles plugin):

- **Never write into `basic/scenes/` or `basic/profiles/` directly.** Stage the files elsewhere and let `ctd_show_apply` copy them. The proc is the only place where the copy and the load happen with no save in between.
- Call `ctd_show_apply` from the plugin's own thread or from the UI thread. Never call it from inside a frontend event callback for a collection or profile change, because that would re-enter `Load()`.

## C2: output guard on the Scene Collection and Profile menus

- `OBSBasic::CtdAnyOutputActive()` (static) returns true when `obs_enum_outputs` finds any `obs_output_active` output. That covers stream, record, replay buffer, virtual camera and every plugin output, including the vertical canvas.
- **Menus:** on `aboutToShow` of `sceneCollectionMenu` and `profileMenu`, `CtdUpdateSwitchMenus()` disables these while any output is active, and enables them again when none is:
  - every collection and profile entry;
  - New, Duplicate, Rename, Import and Remove.
  - Export stays enabled because it is read-only.

  Checking when the menu opens is needed because plugin outputs raise no events. Stock OBS's whole-menu disable of the Profile menu for frontend outputs stays in place.
- **Triggers:** `ChangeSceneCollection()` and `ChangeProfile()` check the guard again before `…_CHANGING` and rebuild the menu if it is blocked. This also covers `obs_frontend_set_current_scene_collection` and `obs_frontend_set_current_profile`, and obs-websocket `SetCurrentSceneCollection` and `SetCurrentProfile`, all of which trigger the same actions.
- **Procs:** `ctd_show_apply` returns `"outputs active"`.
- Follow-up (not in this spike): a tooltip or status-bar hint explaining why the items are greyed out.

## C3: does applying a profile without OBS's restart prompts need a core change?

**No.**

- The prompt is plain frontend code at the end of `OBSBasic::ActivateProfile`. Everything else a profile changes can already be applied at runtime by the frontend: video goes through `obs_reset_video`, and the service, outputs, encoders and hotkeys are rebuilt by `ResetProfileData`.
- The prototype skips the prompt with one optional argument and returns `restart_required` instead.
- The one thing that really needs a restart is a different audio sample rate or speaker layout. libobs `obs_reset_audio2` refuses while outputs are active, and the frontend never resets audio at runtime because sources hold the old audio setup.
- We don't change libobs. **Show Profiles fixes audio at 48 kHz stereo for every show profile**. If a staged profile differs, `restart_required` is true and the plugin tells the operator.

## Vertical canvas: procs on our fork

All three procs are registered on `obs_get_proc_handler()` in `obs_module_post_load` and run on the UI thread (`QMetaObject::invokeMethod(…, BlockingQueuedConnection)` unless already on it). They work on the first canvas dock, which is the only one CTD uses.

Shared file format (UTF-8 JSON) used by export and import:

```json
{
  "format": "ctd-vertical/1",
  "canvas": {
    "width": 1080, "height": 1920,
    "current_scene": "…",
    "streaming_video_bitrate": 6000, "streaming_match_main": false,
    "record_video_bitrate": 6000, "recording_match_main": false,
    "audio_bitrate": 160, "stream_audio_track": 0,
    "stream_advanced_settings": false, "stream_encoder": "…", "stream_encoder_settings": {},
    "record_advanced_settings": false, "record_encoder": "…", "record_encoder_settings": {},
    "file_format": "…", "filename_formatting": "…", "record_audio_tracks": 1,
    "max_size_mb": 0, "max_time_sec": 0,
    "backtrack": true, "backtrack_seconds": 30,
    "stream_delay_enabled": false, "stream_delay_duration": 0, "stream_delay_preserve": false,
    "virtual_camera_mode": 0, "preview_disabled": false,
    "transition": "…", "transitions": [],
    "stream_outputs": [ { "name": "…", "enabled": true, "…per-output encoder settings…": {} } ]
  }
}
```

It uses the same keys as `CanvasDock::SaveSettings()`, but as an **allow-list**. It leaves out:

- `stream_server` and `stream_key` from every stream output. Show files never hold servers or keys;
- `record_path` and `backtrack_path`, which are paths on one machine;
- all `*_hotkey` arrays, which are bindings on one machine;
- `partner_block`, `virtual_cam_warned` and `grid_mode`, which are UI state.

**`void ctd_vertical_export(in string path, out bool success, out string error)`**

- Builds the allow-listed object from the dock (`SaveSettings()`, then filtered) and writes it with `obs_data_save_json_safe(path, "tmp", "bak")`. It works while outputs are running.
- Vertical **scenes** are not in this file. They are in the scene collection, which Show Profiles already stages.

**`void ctd_vertical_import(in string path, out bool success, out string error, out bool resolution_changed)`**

1. **Guards:** the format is `ctd-vertical/1`, and no vertical output is active (stream outputs, record or backtrack). Otherwise it returns `"outputs active"`. An active backtrack (replay buffer) counts as active: stopping it is the caller's job.
2. **Settings:** applies the allow-listed fields to the dock and updates its encoders the same way the config dialog does.
3. **Resolution:** if it changed, uses the config dialog's path: `obs_canvas_reset_video`, then `obs_transition_set_size`, then `LoadScenes()`, then `ProfileChanged()`. In that case `resolution_changed` is true.
4. **`stream_outputs`:** matches by `name`, keeps each existing output's server and key, and adds new outputs with an empty server and key, disabled until `ctd_vertical_set_output` fills them. Outputs that aren't in the file are removed through `LoadStreamOutputs`. Then `UpdateMulti()`.
5. **`current_scene`:** switches to it if the scene exists. This is why import runs **after** `ctd_show_apply`.
6. **Saves `config.json`** (`save_canvas()`).

**`void ctd_vertical_set_output(in string server, in string key, out bool success, out string error)`**

- Sets **output 0** (creating it as `"CTD"` if there are none): `stream_server = server`, `stream_key = key`, `enabled = true`.
- The service type follows the protocol (`whip_custom` if the server contains `whip`, otherwise `rtmp_custom`). If the type changes, the output's service is rebuilt (remove, then re-add through `LoadStreamOutputs`).
- **Guards:** `server` must not be empty, and output 0 must not be active (`"outputs active"`).
- It doesn't start streaming, and it never writes the key to the log.
- It saves `config.json`, where the key stays in plain text exactly as Aitum stores it today. Moving keys out of `config.json` would need its own ticket.

**Show apply order** for the Show Profiles plugin:

1. `ctd_show_apply(collection, profile)` loads the vertical scenes too.
2. `ctd_vertical_import(path)` applies resolution, encoders and the current vertical scene.
3. `ctd_vertical_set_output(server, key)` with values from CTD Relay at go-live time, never from show files.

## Prototype on this branch

| File | What |
|---|---|
| `frontend/widgets/OBSBasic_CtdShow.cpp` | The two procs, `CtdShowApply` and `CtdShowRefresh`, the `ctd-obs` obs-websocket vendor bridge, `CtdAnyOutputActive` and the menu guard |
| `frontend/widgets/OBSBasic.hpp` | Declarations; the `ActivateProfile` optional argument |
| `frontend/widgets/OBSBasic_Profiles.cpp` | Prompt skip when the CTD caller asks for requirements; guard in `ChangeProfile` |
| `frontend/widgets/OBSBasic_SceneCollections.cpp` | Guard in `ChangeSceneCollection` |
| `frontend/widgets/OBSBasic.cpp` | Proc registration in `OnFirstLoad` |
| `frontend/cmake/ui-widgets.cmake` | New source file |
| `test/show-profiles/s1-proto.ps1` | PC test over obs-websocket |

The vertical-canvas procs are specified only; they belong in the plugin fork's own ticket.

**Test on the PC** (CTD OBS from this branch, obs-websocket on):

```powershell
pwsh -File test\show-profiles\s1-proto.ps1 -Password (Read-Host -AsSecureString)
# add -TestGuard to also check the "outputs active" refusal (starts and stops a local recording)
```

The script:

1. Snapshots the active collection to `%TEMP%` as the staged file.
2. Creates a probe scene in OBS that the staged file doesn't have.
3. Calls `show_apply`.
4. Passes only if the probe scene is gone from OBS and from the collection file on disk. If OBS had saved over the staged file first, the probe scene would be back.

It connects only to the local obs-websocket.

## Open points

- The Missing Files dialog during apply (Finding 6).
- The vertical canvas's `config.json` is global. If two show profiles both use the vertical canvas, import must run on every apply. That is already in the order above.
- `CTD-CHANGES.md` gets its Show Profiles row when the real implementation lands, not for this spike.
