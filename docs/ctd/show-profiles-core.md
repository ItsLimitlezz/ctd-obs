# Show Profiles core (T8): `ctd_show_apply`, `ctd_show_refresh` and the switch guard

This is the contract between the CTD OBS core and the Show Profiles code in the CTD plugin (T7). The code is in `frontend/ctd/ctd-show-api.cpp`. Hub design: `epics/show-profiles-design.md` §6–7. Spike findings: [`show-profiles-s1.md`](./show-profiles-s1.md). Where the two docs differ, this one is correct.

## C1: procs on the global proc handler

```
void ctd_show_apply(in string collection, in string profile,
                    out bool success, out string error, out bool restart_required)
void ctd_show_refresh(out bool success, out string error)
```

Both are on `obs_get_proc_handler()`. They are registered **before modules load**, so a plugin can check for them in `obs_module_post_load`:

```c
calldata_t cd = {0};
bool ctdCore = proc_handler_call(obs_get_proc_handler(), "ctd_show_refresh", &cd); /* false on stock OBS */
calldata_free(&cd);
```

A call made before the main window has finished loading returns `success = false` with `"busy: CTD OBS is still starting"`. That probe returns that error too, but `proc_handler_call` still returns true, so the check works.

Both can be called from **any thread**. The work runs on the UI thread: from another thread the call blocks until it's done, and from the UI thread it runs straight away. **Don't** call them from a frontend event callback for a collection or profile change, or while holding a lock the UI thread might need.

### `ctd_show_apply`

| In | Meaning |
|---|---|
| `collection` | Absolute path to a staged scene collection JSON, **outside** `basic/scenes/`. Its `"name"` is the target collection. `""` leaves the collection alone |
| `profile` | Absolute path to a staged profile folder, **outside** `basic/profiles/`. It must contain `basic.ini`; `[General] Name` (or the folder name) is the target profile. Optional files: `service.json`, `streamEncoder.json`, `recordEncoder.json`. `""` leaves the profile alone |

| Out | Meaning |
|---|---|
| `success` | true when everything asked for was applied |
| `error` | `""` on success, otherwise one of the errors below |
| `restart_required` | true when the applied profile changes the audio sample rate or speaker layout. OBS keeps the old audio until it restarts; no prompt is shown. Show Profiles keeps every show at 48 kHz stereo, so this should stay false |

What it does, on the UI thread:

1. **Guards:** not starting up, something to apply, no apply already running and no collection loading, and **no output active** (any `obs_output_active` output, plus `ctd_vertical_get_output` if it reports `active`).
2. **Validates** the staged files: the collection is valid JSON with a `"name"`, the profile has a readable `basic.ini`, and neither is inside OBS's own folders. Nothing has changed yet.
3. **Resolves the targets.** An existing collection or profile with that name is overwritten in place. Otherwise a new one is made the way OBS makes them (safe file name, no collisions). The staged files are copied next to their targets as `*.ctd-tmp`. If anything fails here, the temporary files and any new entries are removed and OBS is untouched.
4. **Profile first**, so video settings are in place before scenes load: `PROFILE_CHANGING`, then the files go into place (a staged folder without an optional file removes that file from the target), then OBS's own `ActivateProfile(…, reset = true)` without the restart prompt. `PROFILE_CHANGED` follows.
5. **Collection:** `SCENE_COLLECTION_CHANGING`, then the staged file goes into place and is loaded straight away, with nothing in between that could save. If the target is another collection, OBS first saves the outgoing one to its own file (stock behaviour, keeps the operator's edits). **If the target is the active collection, nothing is saved**: the in-memory scenes are thrown away and the staged file is loaded. Then `SCENE_COLLECTION_LIST_CHANGED` and `SCENE_COLLECTION_CHANGED`.

All the normal frontend events are delivered (unlike the S1 prototype, which held saving off for the whole apply, and OBS drops every frontend event except `CLEANUP` and `EXIT` while saving is off). So the vertical canvas reloads its scenes on `SCENE_COLLECTION_CHANGED` with no extra work, and T7 can verify on that event (design §6 step 4).

Errors (`error` starts with one of these):

| Error | When |
|---|---|
| `nothing to apply` | both paths empty |
| `busy: CTD OBS is still starting` | before the main window finished loading |
| `busy: a scene collection is loading` | another apply is running, OBS is loading a collection, or a previous clear failed |
| `outputs active` | anything is streaming, recording, buffering, on the virtual camera, or a plugin output (vertical canvas) is running |
| `collection file not found: …`, `collection file must be staged outside basic/scenes: …`, `collection file is not valid JSON: …`, `collection file has no "name": …` | bad staged collection; nothing changed |
| `profile folder not found: …`, `profile folder must be staged outside basic/profiles: …`, `profile folder has no readable basic.ini: …` | bad staged profile; nothing changed |
| `copy failed: …` | a staged file couldn't be copied next to its target; nothing changed |
| `could not move into place: …`, `profile apply failed: …`, `scene collection load failed: …`, `scene collection not found after copy: …` | failed part-way. `(the profile was applied)` is added when only the collection failed. Roll back |
| `the old scene data could not be cleared; restart CTD OBS` | OBS's own clear failed (it then stops saving to protect the data) |

### `ctd_show_refresh`

Rescans `basic/scenes` and `basic/profiles`, rebuilds both menus, and raises `PROFILE_LIST_CHANGED` and `SCENE_COLLECTION_LIST_CHANGED`. It loads and saves nothing. Use it after adding or removing collection or profile files that aren't being applied. It fails with `busy: …` while a load or an apply is running.

The active scene collection always stays in the list, with its in-memory state, even when the rescan doesn't find its file. On a fresh config OBS makes "Untitled" in memory and writes its file with its first queued save, which runs after `FINISHED_LOADING`; the plugin's availability probe calls `ctd_show_refresh` during `FINISHED_LOADING`, so a plain rescan would drop the collection and every later save would fail.

### Rules for T7

- **Never write into `basic/scenes/` or `basic/profiles/` yourself.** Stage outside and let `ctd_show_apply` put the files in place; it is the only place where the copy and the load happen with no save in between.
- **Rollback** (design §6 step 5): the staged-outside rule replaces "rename the staged files into place". Before the apply, call `obs_frontend_save()`, then copy the *current* collection file and profile folder into your staging area as the rollback point. If the apply (or your verify step) fails, call `ctd_show_apply` with those copies. They are outside OBS's folders, so they are accepted.
- Apply order for a show: `ctd_show_apply(collection, profile)`, then `ctd_vertical_import(path)`, then `ctd_vertical_set_output(server, key)` (S1 spec).
- Missing media: if the collection points at files that aren't on this PC, OBS shows its **non-modal** Missing Files window after the load. It doesn't block the apply (S1 Finding 6 thought it did).

### Testing from outside OBS

When obs-websocket is loaded, the core also registers the obs-websocket vendor **`ctd-obs`** with requests `show_apply` (`{collection, profile}`) and `show_refresh` (`{}`). They answer with the same fields as the procs. `test/show-profiles/core-apply.ps1` uses it.

## C2: switching blocked while any output is active

Juan's decision 4 covers every path, not just the Show Profiles dock.

- **What counts as active:** any output for which `obs_output_active` is true (streaming, recording, replay buffer, virtual camera, the vertical canvas's stream/record/backtrack outputs, multitrack, any plugin output), plus the vertical canvas fork's `ctd_vertical_get_output` if it answers `active = true`.
- **Menus:** when the Scene Collection or Profile menu opens while anything is active, a greyed-out first line says **"Stop streaming and recording to switch."** and every collection/profile entry plus New, Duplicate, Rename, Remove (and Import for collections) is disabled. Export stays enabled. When the menu closes, the items are enabled again, so the state is never stale. Stock OBS still greys out the whole Profile menu while a stream, recording, replay buffer or virtual camera is running; the notice shows there when only a plugin output (the vertical canvas) is running.
- **Every other path** goes through the same check: `ChangeSceneCollection` / `ChangeProfile` (the menu actions that `obs_frontend_set_current_scene_collection` / `obs_frontend_set_current_profile` and obs-websocket `SetCurrentSceneCollection` / `SetCurrentProfile` trigger), `obs_frontend_add_scene_collection` / `obs_frontend_create_profile` / `obs_frontend_duplicate_profile` (obs-websocket `CreateSceneCollection` / `CreateProfile`), and removing the active collection or profile. A refused switch logs `[ctd-show] … refused: outputs active` and shows the same message in the status bar for 5 s. There's no modal dialog, so a remote call during a show never pops one up.
- OBS has no hotkeys for switching collections or profiles, so there's nothing to guard there.
- **Never blocked when nothing is active.**
- Not guarded: Export (read-only) and Import Profile (only copies a folder in; it doesn't switch).
