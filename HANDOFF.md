# Handoff: built-in vertical canvas, settings backup, hardware load

Read this first if you are picking up this work, whether you are a person or an AI assistant. It says
what the work is for, what is finished, where it stopped, and what to do next. The full design is in
[`docs/plans/vertical-canvas-backup-hardware-load.md`](docs/plans/vertical-canvas-backup-hardware-load.md).

## About the owner and the goal

- This repository is a personal fork of OBS Studio (`kelleyparker/obs-studio`). The aim is to build
  popular third-party plugin features into OBS itself, so fewer plugins are needed. Built-in
  multistreaming was the first such feature and is already on `master`.
- The owner wants the result to be "as fully functional as possible", with the same features as the
  plugins it replaces.
- **Targets: Windows x64 and macOS Apple Silicon (arm64) only.** Linux, Flatpak, Intel Macs and
  Windows on ARM are not built or tested.
  - The owner's Windows PC is an X299 machine with two NVIDIA GPUs, an RTX 5070 and an RTX 3050.
  - The owner also uses a MacBook Pro with an M1 chip.
  - Code should stay portable (plain Qt and libobs). Anything platform-specific (such as Part C's
    hardware counters) needs a Windows and a macOS implementation.
- The owner tests the builds that CI produces, and may also build locally on the Mac with Xcode.
- **Don't start builds unless the owner asks.** As of 2026-10-15 the owner has said not to build
  yet.

## Where work stopped

| Part | What it is | State |
| --- | --- | --- |
| 0 | CI builds Windows x64 and macOS arm64 on pushes to `claude/**` branches of the fork | Done (macOS added 2026-10-15) |
| B | One-click settings backup and restore (File menu) | **Code done. Not yet compiled on Windows CI and not tested on Windows** |
| A, phase 1 | Built-in vertical canvas (like Aitum Vertical): canvas, settings, docks, recording, replay buffer, multistream, hotkeys | **Code done. Not yet compiled on Windows CI and not tested on Windows** |
| A, phases 2–3 | Full drag-and-resize editing in the vertical preview, transitions, vertical virtual camera, projector, websocket | Not started |
| C | Projected hardware load per output, per GPU; encoder GPU choice per destination | Not started |

Order of work: 0 → B → A phase 1 → C → A phase 2 → A phase 3. Each step should end with a green
Windows x64 CI build that the owner has tested.

### Next steps, in order

1. **Turn GitHub Actions back on.** As of 2026-10-15, Actions is disabled for this repository, so
   nothing builds. Turn it on in **Settings → Actions → General** ("Allow all actions and reusable
   workflows").
2. **Get the branch building on Windows CI.** Open the **Actions** tab, then **Push**, then
   **Run workflow**, and pick this branch. Fix any MSVC errors. Parts B and A (phase 1) have only
   been compile-checked against Qt 6.4 on Linux, so expect a few small MSVC or Qt 6.8 fixes. CI
   treats warnings as errors (`/W3 /WX`).
3. **The owner tests Part B and Part A phase 1** with the CI artifact `obs-studio-windows-x64-*`.
   Use the checklists under "Testing Part B" and "Testing the vertical canvas" below.
4. **Then Part C** (hardware load), then Part A phases 2 and 3. See the plan.

## Branches and workflow

- **Development branch:** `claude/wonderful-hopper-3mrp6t`, which is several commits ahead of `master`.
  Merge it to `master` only after the owner approves.
- **CI:**
  - Pushes to `claude/**` and pull requests build Windows x64 and macOS arm64
    (`.github/workflows/push.yaml`, `pr-pull.yaml`). The push workflow can also be started by hand
    with **Run workflow**.
  - A push that changes only `*.md` files does not start a build. For a new branch, GitHub only
    looks at the newest commit when it decides this.
  - The artifacts to download are `obs-studio-windows-x64-*` and `obs-studio-macos-arm64-*`.
  - The macOS build isn't signed, since the fork has no Apple signing certificate. macOS will
    refuse to open it at first. To allow it, right-click OBS.app → Open, or run
    `xattr -cr /Applications/OBS.app`.
- **Releases:** pushing a `publish/**` branch runs `.github/workflows/fork-release.yaml`. It
  publishes the builds from a given `master` build run as a prerelease. The run id, SHA and tag are
  set in that file.
- **Branches that can be deleted** (already merged or no longer needed): `ci/startup-check`,
  `ci/limit-platforms`, `feature/multistream-max-quality` and `publish/multistream-1`. Claude could
  not delete them (the push was refused), so the owner has to delete them on GitHub.

## Code conventions that CI checks

- Format C and C++ with **clang-format 22** using the repo's `.clang-format`. CI fails on any
  difference. To check locally: `pip install clang-format==22.*`, then
  `clang-format -i <files>`.
- The code is **C++17** (`cmake/common/compiler_common.cmake`). Don't use designated initialisers
  or `char8_t`. With C++17, `std::filesystem::path::u8string()` returns `std::string`.
- New frontend dialogs are built in code with no `.ui` forms, like the multistream dialogs.
- User-visible text goes in `frontend/data/locale/en-US.ini` and is read with `QTStr("Key")`.
- New source files must be added to the lists in `frontend/cmake/ui-*.cmake`.
- Commit messages follow the OBS style, for example `frontend: Add X`, with a short body.

## Part B: settings backup and restore (done, untested)

In the File menu, after "Show Profile Folder":

- **Back Up Settings…** saves a normal `.zip` file containing:
  - profiles;
  - scene collections;
  - app settings (`user.ini`);
  - themes;
  - plugin settings (`plugin_config`, plugin manager).

  A checkbox can leave out stream keys and passwords. The same dialog turns on automatic backups
  each time OBS closes and sets how many to keep.
- **Restore Settings…** reads a backup and lists the profiles and collections in it. It then asks
  which parts to restore and how:
  - **Replace:** the backup replaces those parts of your settings.
  - **Merge:** adds what's missing. Name clashes come in as "Name (Restored)".

  OBS first saves a safety backup to `obs-studio/backups/pre-restore-*.zip` and keeps the last 5. It
  then stages the files and applies them on the next start, before any config is loaded. If applying
  fails, it rolls back from the safety backup. Restoring is blocked while outputs are active.

| File | Purpose |
| --- | --- |
| `frontend/utility/ZipArchive.{hpp,cpp}` | Minimal zip writer and reader with no compression. Uses libobs `calc_crc32` and refuses unsafe entry paths. Tested standalone (with `unzip -t` and Python `zipfile`) |
| `frontend/utility/SettingsBackup.{hpp,cpp}` | Backup, inspection, staging and applying a restore, plus automatic backups (namespace `SettingsBackup`) |
| `frontend/dialogs/OBSSettingsBackup.{hpp,cpp}` | `OBSBackupSettingsDialog` and `OBSRestoreSettingsDialog` |
| `frontend/widgets/OBSBasic_Backup.cpp` | `OBSBasic::SaveForBackup`, `OpenBackupSettings`, `OpenRestoreSettings` |
| `frontend/widgets/OBSBasic.cpp` | Adds the menu entries in `OBSInit`. Runs the automatic backup once at the end of `saveAll()` |
| `frontend/OBSApp.cpp` | `ApplyPendingRestore` runs in `InitGlobalConfig` just before `InitUserConfig`. Defaults for `[Backup]` keys |

Change from the plan: the automatic backup options are in the Back Up Settings dialog, not in
Settings → General.

### Testing Part B (owner, on Windows)

- [ ] Back up everything. The zip opens in Windows Explorer and has `manifest.json`.
- [ ] Back up without stream keys. The keys in `profiles/*/service.json` inside the zip are empty.
- [ ] Delete a profile and a scene collection and change a setting. Restore with **Replace** and
      restart. Everything comes back.
- [ ] Restore with **Merge**. The duplicates appear as "… (Restored)".
- [ ] `%APPDATA%\obs-studio\backups\pre-restore-*.zip` exists after a restore.
- [ ] Profile and collection names with non-English characters survive a backup and restore.
- [ ] Turn on automatic backups, close OBS, and check that a new "OBS Auto Backup …" zip appears.

## Part A phase 1: vertical canvas (done, untested)

**Tools → Vertical Canvas…** turns the vertical canvas on and sets:

- its base and scaled resolution (default 1080x1920) and downscale filter;
- the recording path, format, encoder (automatic = NVENC, QSV, AMF or x264, in that order),
  bitrates, audio track and filename suffix;
- the replay buffer length and size.

The first time it is turned on, three docks appear on the right:

- **Vertical Preview:** the vertical picture, with buttons to start and stop recording and the
  replay buffer, and to save a replay.
- **Vertical Scenes:** add, remove, rename (double-click), duplicate and reorder scenes. Clicking a
  scene switches to it.
- **Vertical Sources:** the sources of the current vertical scene. The tick shows or hides a
  source.
  - **+** adds a new source of any type, an existing source, or a scene from the main canvas.
  - Sources can be removed, reordered, and opened in Properties or Filters.
  - **Transform** offers Fit, Fill (crop), Stretch, Center and Reset.
  - New sources are fitted to the canvas.

There are also:

- **Multistream output choice:** each multistream destination has an **Output** setting: the main
  canvas or the vertical canvas. Vertical destinations always get their own encoder.
- **Hotkeys** (Settings → Hotkeys): start and stop vertical recording, start and stop the vertical
  replay buffer, and save a vertical replay.

| File | Purpose |
| --- | --- |
| `libobs/obs-canvas.c`, `libobs/obs.h` | New `obs_canvas_clear_video()`, which drops a canvas's video mix but keeps its scenes. Used when vertical is turned off |
| `frontend/utility/VerticalCanvas.{hpp,cpp}` | The canvas: enabled and resolution (profile `[Vertical]`), scenes and their order, current scene, save and load with the scene collection (`"vertical_canvas"` key) |
| `frontend/utility/VerticalOutput.{hpp,cpp}` | Vertical recording and replay buffer, with their own encoders. Settings live in profile `[Vertical]` |
| `frontend/dialogs/OBSVerticalSettings.{hpp,cpp}` | The Tools → Vertical Canvas dialog |
| `frontend/docks/VerticalDocks.{hpp,cpp}` | The Preview, Scenes and Sources docks |
| `frontend/widgets/OBSBasic_Vertical.cpp` | The OBSBasic glue: settings dialog, start and stop helpers for hotkeys |
| `frontend/widgets/OBSBasic*.cpp` | Hooks in `OBSInit`, `Save`, `LoadData`, `ClearSceneData`, `ResetProfileData`, `ResetVideo`, `ResetUI`, hotkeys and close |
| `frontend/utility/MultistreamOutput.*`, `frontend/dialogs/OBSMultistreamSettings.*`, `frontend/docks/MultistreamDock.cpp` | The `vertical` flag (`"canvas"` in `multistream.json`) and the Output choice |

Design decisions:

- **No double audio:** the vertical canvas is created with `ACTIVATE | SCENE_REF` and without
  `MIX_AUDIO`, so a microphone in both canvases isn't heard twice. Vertical recordings and streams
  use the same audio mix as the main outputs. Audio sources that are only in vertical scenes are
  not heard. This is shown in the settings dialog.
- **Settings in their own dialog:** the vertical settings are in their own dialog, not in
  Settings → Video and Output as the plan said. This is simpler and keeps the big settings dialog
  untouched.
- **Phase 1 editing:** the preview can't be dragged or resized yet. That is phase 2.
- **Frame rate and color:** the vertical canvas uses the main canvas's frame rate and color
  settings. libobs requires the same frame rate.
- **Portrait multistream limits:** for multistream, a platform's landscape limits (for example
  1920x1080) are turned on their side for portrait video. Bitrates are looked up by the short side,
  so 1080x1920 counts as 1080p.

Known limits and risks to check on Windows:

- Switching scene collections while vertical outputs are live stops them (forced). Check that OBS
  doesn't crash when you do this.
- Turning vertical off, or changing its resolution, is refused while a vertical output or vertical
  multistream destination is live.

### Testing the vertical canvas (owner, on Windows)

- [ ] Tools → Vertical Canvas, tick Enable, OK. Three vertical docks appear and show a black
      1080x1920 preview.
- [ ] Add an existing source (for example the webcam) and a main canvas scene. Both show in the
      vertical preview, fitted. Try Fill, Stretch, Center and Reset.
- [ ] Add, rename, duplicate, reorder and remove vertical scenes. Clicking a scene switches the
      preview.
- [ ] Restart OBS. The vertical scenes, their order and the current scene are restored.
- [ ] Switch to another scene collection and back. Vertical scenes stay with their collection.
- [ ] Switch profiles. Vertical on/off and resolution follow the profile.
- [ ] Record vertical and main at the same time. The vertical file is 1080x1920 at the main frame
      rate, with audio, and is named "... Vertical".
- [ ] Start the vertical replay buffer, save a replay (button and hotkey). The status bar shows
      the file.
- [ ] Add a multistream destination with Output = Vertical canvas and go live. It streams 9:16
      while the main stream is 16:9.
- [ ] Turn vertical off while idle: the docks show "turned off", scenes come back when turned on.
      Try turning it off while recording: it is refused.
- [ ] A microphone that is in both canvases is heard once in the vertical recording.
- [ ] Main preview, scenes, sources, recording, streaming and replay buffer behave as before.

## Notes for the remaining Part A work

- **Canvas API:** `libobs/obs.h` (`obs_canvas_*`) and `libobs/obs-canvas.c`. The flags are named
  `ACTIVATE`, `MIX_AUDIO`, `SCENE_REF`, `EPHEMERAL` (enum `obs_canvas_flags`).
- **Frontend canvas helpers:** `OBSBasic::AddCanvas/RemoveCanvas` in
  `frontend/widgets/OBSBasic_Canvases.cpp`, and the `OBS::Canvas` wrapper in
  `frontend/utility/OBSCanvas.*`. Canvases are saved in the scene collection under `"canvases"`
  (`frontend/widgets/OBSBasic_SceneCollections.cpp`).
- **Binding an encoder to a canvas:**
  `obs_encoder_set_video(enc, obs_canvas_get_video(canvas))`. See
  `frontend/utility/MultitrackVideoOutput.cpp`.
- **Things to watch out for:**
  - `obs_save_canvas` doesn't save the resolution; `VerticalCanvas` keeps it in the profile and
    calls `obs_canvas_reset_video`.
  - The canvas frame rate always follows the main canvas.
  - `obs_get_source_by_name` and `obs_enum_scenes` only see the main canvas.
  - Canvas scenes emit `source_create_canvas`, not `source_create`, so they don't show up in the
    main Scenes dock.
- **Phase 2** (full editing) means making `OBSBasicPreview`, `SourceTree` and
  `OBSBasicTransform` work against a canvas other than the main one. See the plan.

## Part C (hardware load)

Not started. The plan covers it in full:

- live GPU, CPU, network and disk counters through Windows PDH;
- NVIDIA details through `nvml.dll`, loaded at runtime;
- matching NVENC device numbers to DXGI adapters by PCI bus id;
- per-output load estimates that are calibrated against measured load;
- a Hardware Load dock with one stacked bar per component;
- an "Encoder GPU" choice for each multistream destination and for vertical recording.
