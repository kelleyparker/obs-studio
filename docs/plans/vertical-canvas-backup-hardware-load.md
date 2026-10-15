# Built-in Vertical Canvas, Settings Backup & Restore, Hardware Load Projection

This plan has two parts:
- **Part A** is the vertical canvas.
- **Part B** is one-click backup and restore of settings.

Part B is smaller and doesn't depend on Part A, so it ships first. It also gives users a safety net before the large vertical-canvas changes land.

**Target: Windows x64 only for now.** Other platforms aren't built, tested or worried about. The code stays plain Qt and libobs with no Windows-only APIs, so other platforms can be added later. CI is the only build gate, because the cloud container is Linux and can't build or run the Windows app.

# Part 0: CI scoped to Windows x64 (first commit)
- `.github/workflows/push.yaml:25` passes `platforms: ${{ github.repository_owner == 'obsproject' && 'windows macos ubuntu flatpak' || 'windows' }}` to `build-project.yaml`. This copies what `pr-pull.yaml:26` already does, so pushes to the fork build only Windows x64. Today x64 is already the only Windows architecture.
- `scheduled.yaml:96` gets the same change.
- The fork-release workflow is left alone unless it builds non-Windows targets. If it does, those are limited to Windows x64 the same way.
- Every push then produces a Windows x64 build artifact (`obs-studio-windows-x64-*`) that the user downloads and tests on their PC.

# Part A: Vertical Canvas

## Context
The fork aims to reduce third-party plugins by building features into OBS. Its multistreaming was built in that way (commits 063a8c9..fa720f0). This change does the same for Aitum's Vertical Canvas plugin. Vertical canvas adds a second 9:16 video mix with its own scenes and its own recording, replay buffer and streaming.

The user's goal is Aitum feature parity: "as fully functional as possible". Their answers to the earlier questions fix the approach:
- **Write it natively.** No Aitum code is ported.
- **Stream through the existing multistream feature.** Each destination gets a Horizontal/Vertical output choice.
- **Include in the first version:** preview, scenes and sources, recording, streaming and replay buffer.

libobs already has the core of this, but nothing in the frontend uses it yet:
- **Canvas API** in `libobs/obs.h:2638-2730`, `libobs/obs-canvas.c`.
- **Frontend canvas management:**
  - `OBSBasic::AddCanvas/RemoveCanvas/ClearCanvases` in `widgets/OBSBasic_Canvases.cpp`.
  - The `OBS::Canvas` RAII wrapper in `utility/OBSCanvas.{hpp,cpp}`.
- **Scene collection save/load:** canvases and their scenes, via `canvas_uuid`, in `widgets/OBSBasic_SceneCollections.cpp:873-902,1266`.
- **Pattern for binding an encoder to a canvas:** `utility/MultitrackVideoOutput.cpp:305` uses `obs_encoder_set_video(enc, obs_canvas_get_video(canvas))`.

Constraints found during exploration:
- `obs_save_canvas` does not save the resolution. We store our own `obs_video_info` and call `obs_canvas_reset_video` after loading.
- The canvas FPS always follows the main canvas (`libobs/obs.c:614`).
- The canvas API is marked unstable.
- `obs_get_source_by_name` and `obs_enum_scenes` only see the main canvas. Use the `obs_canvas_*` lookups instead.
- Scenes created on another canvas emit `source_create_canvas` on that canvas's own signal handler, not the global `source_create`. As a result, the main scene list stays clean.
- `OBSBasicPreview`, `SourceTree*`, `OBSBasicTransform` and `OBSBasicSourceSelect` call `main->GetCurrentScene()` directly, so they only work on the main canvas.

All paths below are relative to `frontend/` unless noted.

## Phase 1: Core (one commit per bullet group)

### 1a. VerticalCanvas manager: `utility/VerticalCanvas.{hpp,cpp}`
- Singleton-ish object owned by `OBSBasic` as `std::unique_ptr<VerticalCanvas> vertical`, the same way `multistream` is owned.
- **Enable:** `OBSBasic::AddCanvas("Vertical", &ovi, OBS_CANVAS_ACTIVATE | OBS_CANVAS_MIX_AUDIO | OBS_CANVAS_SCENE_REF)`.
  - `ovi` is copied from `obs_get_video_info()` with the base/output size set to the vertical settings (default 1080x1920).
- **Disable:** `RemoveCanvas`.
- **Current scene:** `SetCurrentScene(OBSScene)` calls `obs_canvas_set_channel(canvas, 0, scene_source)`. Version 1 cuts between scenes; transitions come in phase 3.
- **Scene list:**
  - Built from `obs_canvas_enum_scenes`.
  - Kept in sync with the canvas signals `source_add`, `source_remove` and `source_rename`, plus the global `canvas_remove`.
  - Changes are exposed as Qt signals: `ScenesChanged`, `CurrentSceneChanged`.
- **Persistence:**
  - Settings that belong to the profile go in the profile config section `[Vertical]`: enabled, base and output resolution, scale type.
  - Settings that belong to the scene collection go under a new top-level `"vertical_canvas"` key in the scene collection JSON:
    - `canvas_uuid`
    - `current_scene`
    - `scene_order`
  - Hook point for saving: `OBSBasic::Save` next to `"canvases"` (`OBSBasic_SceneCollections.cpp:900`).
  - Hook point for loading: `LoadData`, after `LoadCanvases` (:1266) and after the sources load.
  - On load, find the canvas by uuid and call `obs_canvas_reset_video`.
  - If the collection has no vertical canvas but the profile has it enabled, create the canvas and one default "Vertical Scene".
- **Video resets:**
  - After `OBSBasic::ResetVideo` (FPS or colour format changes), re-apply `obs_canvas_reset_video` with the main canvas's FPS and colour settings.
  - Block resolution changes while vertical outputs are active, because `reset_video` fails when the canvas's video is active.

### 1b. Settings: Settings > Video, new "Vertical Canvas" group (`settings/OBSBasicSettings_Video.cpp` or similar)
- **Video:** an Enable checkbox, base and output resolution combos with presets (1080x1920, 720x1280, 1440x2560), and the downscale filter.
- **New "Vertical" section on the Output page (Simple and Advanced):**
  - Recording: path, format, encoder, bitrate/CQ, audio track.
  - Replay buffer: enable, max seconds, max MB.
  - Filename formatting, defaulting to the main formatting with a ` Vertical` suffix.
- All values are stored in profile `[Vertical]` keys and read through `config_get_*` on the active profile config.

### 1c. Outputs: `utility/VerticalOutput.{hpp,cpp}`
- **Recording output:** an `ffmpeg_muxer`.
  - Its video encoder is set with `obs_encoder_set_video(enc, obs_canvas_get_video(canvas))`.
  - Its audio encoder is the AAC/Opus encoder created the same way as in `SimpleOutput` (`CreateSimpleAACEncoder`) and bound to `obs_get_audio()`.
- **Replay buffer:** a `replay_buffer` output that shares those encoders.
- **Settings reused from `utility/SimpleOutput.cpp:836` (`ConfigureRecording`):** directory, format, extension, `allow_spaces`, `max_time_sec`, `max_size_mb`.
- **Filenames:** reuse `GetOutputFilename`, `GetFormatString` and `GetFormatExt` (`OBSApp.cpp:1571-1692`) and the auto-remux logic in `BasicOutputHandler::GetRecordingFilename` (`utility/BasicOutputHandler.cpp:441`).
- **Stop and error handling:** start/stop/error signals mirror the `BasicOutputHandler` callback pattern.
- **Integration with existing code:**
  - `OBSBasic::Active()` and `ResetOutputs` (`widgets/OBSBasic_OutputHandler.cpp:33,53`) include `vertical->Active()`, just as they do for multistream.
  - Frontend events go to the main recording and replay buffer status. New events: `VERTICAL_RECORDING_STARTED/STOPPED` and `VERTICAL_REPLAY_BUFFER_*`.
  - A "Saved vertical replay" status bar message.

### 1d. Docks: `docks/VerticalPreviewDock`, `docks/VerticalScenesDock`, `docks/VerticalSourcesDock`
These follow the built-in `multistreamDock` pattern:
- created in `widgets/OBSBasic.cpp:262` and set up with `SETUP_DOCK`;
- listed in `IsDockObjectNameUsed` and `on_resetDocks_triggered` (`widgets/OBSBasic_Docks.cpp:92,210`);
- in Reset UI, they are placed to the right of the main preview.

**Preview dock:**
- An `OBSQTDisplay` with a draw callback that calls `obs_canvas_render(canvas)`, letterboxed with `GetScaleAndCenterPos` (`utility/display-helpers.hpp:27`). This is the same structure as `OBSBasic::RenderMain` (`widgets/OBSBasic_Preview.cpp:132`).
- A toolbar with Record (shows elapsed time), Replay buffer on/off, Save replay, Settings, and a status label.

**Scenes dock:**
- A `QListWidget` with add, remove, rename, duplicate, and up/down/drag reordering.
- Clicking a scene sets it as the current scene.
- New scenes are created with `obs_canvas_scene_create`.

**Sources dock:**
- Lists the items of the current vertical scene, with a visibility toggle, lock, remove and reorder.
- Add menu:
  - "Add existing source", picked from all public sources.
  - "Add main canvas scene", which nests a horizontal scene.
  - Create new source of any type: create it with `obs_source_create` and open its properties.
- Context menu:
  - Properties and Filters, reusing `OBSBasic::CreatePropertiesWindow/CreateFiltersWindow`.
  - Transform presets (Fit, Fill, Stretch, Center, Reset) through `obs_sceneitem_set_bounds_type/alignment`.

### 1e. Multistream: Horizontal/Vertical destination output (`utility/MultistreamOutput.{hpp,cpp}`, `dialogs/OBSMultistreamSettings.cpp`)
- **Data:** add `canvas` (`"main" | "vertical"`) to `MultistreamDestination`, persisted in `multistream.json`. If missing, it defaults to `"main"`.
- **Encoders:** vertical destinations always use their own encoder, even in Shared mode, because shared encoders are bound to the main video.
  - `CreateVideoEncoder` (:628) uses `obs_canvas_get_video(vertical canvas)` instead of `obs_get_video()` (:706).
  - `ResolveMultistreamQuality` (:267) uses the vertical canvas's `obs_canvas_get_video_info` for resolution limits.
- **Dialog:** an "Output: Horizontal / Vertical" combo, disabled with a tooltip when the vertical canvas is off. Vertical destinations get a TikTok/Shorts preset where limits are known.
- **Dock:** vertical rows get a "Vertical" tag.
- Vertical destinations still go live together with the main stream; that does not change.

## Phase 2: Full interactive editing (parity with the main canvas)
Introduce a small `CanvasContext` interface with:
- `GetCurrentScene()`
- `GetCanvas()`
- base size
- `undo_stack&`

`OBSBasic` implements it for the main canvas, and `VerticalCanvas` for vertical.

Change these to take a `CanvasContext*` instead of calling `main->GetCurrentScene()` / `obs_get_video_info()` directly:
- `widgets/OBSBasicPreview.cpp` (about 158 sites)
- `components/SourceTree*.cpp`
- `dialogs/OBSBasicTransform.cpp:110-132`
- `dialogs/OBSBasicSourceSelect.cpp:160,205,770,831`

The main canvas behaviour must not change.

Then:
- Swap the phase-1 preview and sources docks for a real `OBSBasicPreview` and `SourceTree` bound to the vertical context. That brings drag, resize, crop, snapping, multi-select, nudge, groups, undo/redo and Edit Transform.

## Phase 3: Remaining Aitum features
- **Scene transitions:** a vertical transition plus duration, using a transition source on canvas channel 0.
- **Hotkeys:**
  - Start/stop vertical recording.
  - Pause recording.
  - Replay buffer and save replay.
  - Per vertical scene "switch to".
  - Split file and chapter marker, using the same mechanics as the main recording.
- **Recording features:** pause and split file, mirroring `AdvancedOutput`.
- **Vertical virtual camera:** a Start Vertical Virtual Camera action, using `virtualcam_output` with `obs_output_set_media(out, obs_canvas_get_video(...), obs_get_audio())`.
- **Main-scene linking:** an optional per-horizontal-scene "linked vertical scene", stored in the vertical section of the collection JSON. Switching the main scene switches vertical.
- **Projectors:** add a "Vertical canvas" projector option in `widgets/OBSProjector.cpp`, rendering with `obs_canvas_render`.
- **obs-websocket:** vendor requests for status, scene list and switching, recording, replay buffer and saving replays (`plugins/obs-websocket`). Aitum vendor names are left out.
- **Studio mode for the vertical canvas** is optional and comes last.

## Risks / notes
- **Doubled audio:** a source that is in both canvases might be mixed twice when the canvas has `MIX_AUDIO`. Verify early in phase 1.
  - If audio does double, create the canvas without `MIX_AUDIO`. Vertical-only audio sources then need to be global audio sources, or we add a wrapper similar to Aitum's.
- **Profile vs collection split:** the resolution is per profile and the scenes are per collection. A collection that is loaded with a profile that has vertical disabled keeps its vertical scenes, but the canvas has no video mix until vertical is enabled.
- **Locale strings:** new strings go under `Vertical.*` in `data/locale/en-US.ini`.
- **Build files:** new files go in `cmake/ui-utility.cmake`, `ui-docks.cmake` and `ui-dialogs.cmake`.
- **Dialogs** are built in code, like the multistream ones, with no `.ui` forms.

## Verification (Part A)
- **Build:** after each sub-phase, push to `claude/wonderful-hopper-3mrp6t` and wait for the Windows x64 CI job to go green. CI fixes are done before moving on.
- **Before each push:** run clang-format on the changed files (the repo's `.clang-format`) to match CI's format check, and re-read the diff for MSVC-specific pitfalls such as designated initializers and missing includes.
- **Manual test (the user, on the Windows x64 CI artifact):**
  - Enable vertical and add vertical scenes and sources.
  - Restart OBS and confirm the scenes, current scene and resolution are restored.
  - Switch scene collections and profiles.
  - Record vertical and horizontal at the same time. The vertical file should be 1080x1920 with the same FPS and audio.
  - Save a replay, then stream to a vertical multistream destination together with the main stream.
  - Disable vertical while idle, and confirm it is blocked while recording.
- **Regression:**
  - Main preview editing, the scenes and sources docks, and main recording, streaming and replay buffer are unchanged.
  - Existing `multistream.json` files without `canvas` still load.
- **Delivery:** commit per sub-phase on `claude/wonderful-hopper-3mrp6t`, push, and check that the Windows x64 CI job is green. Merging to master happens only after the user approves.

# Order of work
1. Part 0: CI scoped to Windows x64.
2. Part B: backup and restore.
3. Part A phase 1: vertical core.
4. Part C: hardware load and per-destination encoder GPU. It needs the vertical outputs and multistream in place.
5. Part A phase 2: interactive editing.
6. Part A phase 3: remaining Aitum features.

Each step ends with a green Windows x64 build on the branch, and the user tests it before the next step starts.

# Part B: Settings Backup & Restore (implement first)

## Context
To back up OBS settings today, the user has to open File → Show Settings Folder, go up a level, and zip `obs-studio` by hand. Restoring is just as manual and error-prone. For example, files that OBS has open get overwritten while it is running.

The goal is a built-in, guided backup and restore that produces a normal `.zip`. The archive should open in any zip tool and need no third-party plugin or library.

Existing pieces to reuse:
- Config root: `GetAppConfigPath(path, size, "obs-studio/...")` (`frontend/OBSApp.cpp:1766`). It is already portable-mode aware.
- Startup order: `OBSApp::AppInit` (`frontend/OBSApp.cpp:1126`) runs `MakeUserDirs` and then `InitGlobalConfig`. A pending restore is applied before `InitGlobalConfig`.
- Saving state first: `OBSBasic::SaveProjectNow()` and the profile config save, so the backup is current.
- Checksums: `calc_crc32` (`libobs/util/crc32.h`). Check that it matches the ZIP CRC-32: crc32("123456789") should be 0xCBF43926.
- Menu: File menu in `frontend/forms/OBSBasic.ui:720`, next to `actionShowSettingsFolder`.

## Design
1. **`frontend/utility/ZipArchive.{hpp,cpp}`**
   - A small zip writer and reader, about 300 lines, in "stored" mode, which means no compression. It uses the local headers, central directory and EOCD records, plus UTF-8 names (flag bit 11).
   - The reader also inflates deflate entries using zlib, but only if zlib is already linked by the frontend; otherwise stored-only. Then a backup that was re-zipped with an OS tool can still be restored.
   - Config files are small text, so no compression is acceptable.
   - Entry paths are sanitised on read: anything containing `..`, any absolute path and any drive letter is rejected.
2. **`frontend/utility/SettingsBackup.{hpp,cpp}`**
   - **Create backup**
     - Gathers these from the config root:
       - `global.ini` / `user.ini`
       - `basic/profiles/**`, which includes `multistream.json` and the vertical keys
       - `basic/scenes/*.json`
       - `themes/**`
       - `plugin_config/**`
     - **Always excluded:**
       - `logs/`, `crashes/`, `updates/`, `profiler_data/`
       - `*.bak` / `*.tmp`
       - Browser caches: `plugin_config/obs-browser/{Cache,Code Cache,GPUCache,...}`
       - Any single file over 50 MB, with a warning
     - Writes a `manifest.json` that records the OBS version, date, OS, portable flag, entry list, and the user's options.
   - **Inspect backup:** reads the manifest and lists its profiles and scene collections for the restore dialog.
   - **Stage restore**
     - Before anything is changed, the current config is saved automatically to `obs-studio/backups/pre-restore-<date>.zip`.
     - The chosen entries are extracted to `obs-studio/.restore-pending/`.
     - Writes `restore.json` with the mode:
       - **Replace:** wipes the backed-up categories, then copies.
       - **Merge:** adds profiles and collections that don't exist yet. Name clashes are imported as "Name (Restored)".
   - **Apply pending restore:** called at the top of `OBSApp::AppInit`, before `InitGlobalConfig`.
     - Moves the staged files into place and deletes the staging folder.
     - Logs what changed.
     - If anything fails, it restores from the safety zip.
3. **UI: File → "Back Up Settings…" and "Restore Settings…"** (new actions in `OBSBasic.ui`, handlers in a new `widgets/OBSBasic_Backup.cpp`)
   - **Backup dialog** (`dialogs/OBSSettingsBackup.{hpp,cpp}`, built in code like the multistream dialogs):
     - Checkboxes: Profiles, Scene collections, App settings, Themes, Plugin settings.
     - "Include stream keys", on by default, with a warning that the file then contains secrets. When it is off, `key` fields in `service.json` and `multistream.json` are blanked inside the zip.
     - Save location, defaulting to Documents with the name `OBS Backup YYYY-MM-DD HHMM.zip`.
     - A progress bar, and a final "Show in folder" button.
   - **Restore dialog:**
     - Pick a zip, then see a summary of its date, OBS version and the profiles and collections it contains.
     - Choose Replace or Merge.
     - Confirm, then OBS offers "Restart now". The restore runs on that restart because profiles and collections are in use, and it reuses the existing restart flow (`OBSBasic` already restarts after settings changes such as language).
     - Restoring is blocked while outputs are active.
4. **Automatic backups (optional)**
   - Settings → General → "Backups" group, stored in user config:
     - "Automatically back up settings on exit", off by default.
     - "Keep last N", default 5.
     - A folder picker.
   - Runs at shutdown, after the final save in `OBSBasic::closeEvent`, and prunes old automatic backups.
5. **Housekeeping**
   - Locale keys go under `Backup.*` in `data/locale/en-US.ini`.
   - New files go in the `cmake/ui-*.cmake` lists.

## Verification (Part B)
- Unit-style check of `ZipArchive`: compile it standalone in the container. It depends only on the crc32 code (copied for the test) and the C++ std library. Round-trip write and read, confirm the output opens with `unzip -t` and Python's `zipfile`, and check that crafted `../` entries are rejected.
- Windows check: the zip opens in Windows Explorer (the user, on the CI artifact).
- Manual round trip:
  1. Back up.
  2. Delete a profile and a collection, change a setting.
  3. Restore with Replace and restart. Everything comes back.
- Merge restore: the renamed duplicates show up.
- The pre-restore safety zip exists.
- Portable mode uses the portable config folder.
- Windows paths: a backup with non-ASCII profile names round-trips, and the default location is Documents.
- With stream keys excluded, the keys are blank in the zip, and restoring that zip doesn't wipe existing keys in Merge mode.
- Commit and push on `claude/wonderful-hopper-3mrp6t`, and check that the Windows x64 CI job is green.

# Part C: Projected hardware load per output (multi-GPU aware)

## Context
The user streams from an X299 machine with two GPUs, an RTX 5070 and an RTX 3050. They want to see, for each enabled output, how much it is expected to add to the load on each component, including each GPU separately. That way they can balance configurations, for example by putting the vertical and multistream encodes on the 3050, before they go live.

Today OBS shows only overall CPU, dropped frames and render lag (`widgets/OBSBasicStats.cpp`). It can't pick an NVENC GPU per multistream destination either. obs-nvenc supports this through its `gpu` / `device` setting (`plugins/obs-nvenc/nvenc-compat.c:25,116`).

## Design
1. **`utility/HardwareInfo.{hpp,cpp}`: what the machine has (Windows)**
   - **GPUs:**
     - Listed through DXGI with `CreateDXGIFactory1` and `EnumAdapters1`, giving name, LUID, dedicated VRAM and vendor. Software adapters are skipped. This is the same approach as `libobs-d3d11/d3d11-subsystem.cpp:951`.
     - The render GPU is the adapter chosen in Settings → Advanced (`Video/AdapterIdx`).
   - **NVIDIA extras:** come from `nvml.dll`, which ships with the NVIDIA driver and is loaded at runtime with `LoadLibrary`, so there's no build dependency:
     - compute capability, which identifies the NVENC generation;
     - encoder session count;
     - PCI bus id.
     - **The PCI bus id matters** because obs-nvenc's `device` index uses CUDA's ordering, and CUDA's ordering can differ from both NVML's and DXGI's. NVENC device N is mapped to its DXGI adapter by comparing PCI bus ids: `cuDeviceGetPCIBusId` against `nvmlDeviceGetPciInfo`. Without this, "GPU 1" could be labelled with the wrong card.
   - **CPU:** model, cores and threads. **System:** RAM.
2. **`utility/HardwareMonitor.{hpp,cpp}`: live, measured load (Windows PDH, vendor-neutral, the same counters Task Manager uses)**
   - **Counters:**
     - Per GPU, matched by LUID: `\GPU Engine(*engtype_3D)\Utilization Percentage` and `(*engtype_VideoEncode)`, plus `\GPU Adapter Memory(*)\Dedicated Usage`.
     - Overall: `\Processor Information(_Total)\% Processor Utility` and RAM in use.
     - Network: `\Network Interface(*)\Bytes Sent/sec`.
     - Disk writes: `\PhysicalDisk(_Total)\Disk Write Bytes/sec`.
   - **Per process:** OBS-only GPU use comes from filtering the GPU Engine instances by `pid_<obs pid>`. OBS CPU use comes from the existing `os_cpu_usage_info`.
   - Sampled once a second on a worker thread and only while a load view is open or outputs are active, so it costs nothing otherwise.
3. **`utility/LoadEstimator.{hpp,cpp}`: projected contribution of each output**
   - **Outputs covered:**
     - main stream and main recording;
     - each enabled multistream destination;
     - vertical recording and vertical replay buffer;
     - each canvas's composition, which is render-GPU 3D work.
   - **Inputs per output:** encoder type (NVENC, x264, QSV, AMF), preset, resolution, FPS, bitrate, which GPU encodes it, and whether its encoder is shared.
   - **Video encode cost per output:** pixel rate (w×h×fps) multiplied by a per-preset factor and compared with the capacity of the chosen engine.
     - NVENC: capacity comes from a small built-in table by NVENC generation. It is shown as "estimate" until calibrated.
     - x264: the CPU cost depends on core count.
   - **Other loads per output:**
     - **Shared encoders** (shared multistream mode) count once.
     - **Encoding on a GPU other than the render GPU** adds a CPU readback and a PCIe-copy cost on the CPU and on the render GPU's copy engine. The UI says so.
     - **VRAM:** each NVENC session adds an estimate.
     - **Upload bandwidth:** video plus audio bitrate plus about 5% overhead. Recordings add to disk writes instead.
   - **Calibration:**
     - Whenever outputs are running, the measured utilisation from item 2 is divided by the active pixel rate per engine. The result is stored per GPU and encoder in `user.ini`, as a moving average.
     - Later projections use the stored factor, and their label changes from "estimate" to "calibrated".
     - A "Run calibration" button in the load view runs short synthetic encodes, about 5 s per GPU and preset, with test encoders on a color-bars source. Users can calibrate before ever going live.
   - **Warnings:**
     - any component projected above 85%;
     - more NVENC sessions than the GeForce driver allows on one GPU;
     - upload above a user-entered upload speed (Settings → Advanced);
     - an encode running on a GPU other than the render GPU.
4. **UI**
   - **"Hardware Load" dock** (`docks/HardwareLoadDock.{hpp,cpp}`), a custom `QPainter` widget with no chart library:
     - One horizontal stacked bar per component:
       - CPU;
       - for each GPU, "RTX 5070 – 3D", "RTX 5070 – Encoder" and "VRAM";
       - the same three for the RTX 3050;
       - RAM;
       - Upload;
       - Disk.
     - Each output gets its own color segment, labelled e.g. "Main stream", "Twitch (separate)", "Vertical recording". A legend lists each output and what it adds.
     - A thin "measured" marker on each bar shows the live value, so the projection can be compared with reality.
     - The tooltip on a segment shows the numbers and their basis (estimate or calibrated).
     - The dock updates immediately when settings change, so it's useful before going live.
   - **Inline in the multistream destination dialog and the vertical output settings:** a compact "Projected load" strip for that one configuration, e.g. "+18% RTX 3050 encoder, +6.2 Mbps upload".
   - **New "Encoder GPU" combo** for each separate-mode multistream destination and for vertical recording.
     - It lists NVIDIA GPUs by name, mapped through the PCI-bus-id mapping in item 1.
     - It writes the obs-nvenc `gpu` setting.
     - It is hidden when there's only one NVENC device.
     - New fields: `encoder_gpu` per destination in `multistream.json`, and `[Vertical] EncoderGPU`.
   - **Placement:** the load view is in Tools → "Hardware Load…", next to Multistreaming, and in the Docks menu.
5. **Housekeeping:**
   - Locale keys under `HardwareLoad.*`.
   - CMake lists updated.
   - `pdh.lib` is linked only in the Windows section of `frontend/cmake/os-windows.cmake`.
   - On other platforms the feature compiles out with a "Windows only for now" stub.

## Verification (Part C)
- **Unit-style:** feed `LoadEstimator` synthetic configurations in a small standalone test built in the container, and check:
  - totals and stacking;
  - shared encoders counting once;
  - the cross-GPU penalty being applied;
  - calibration maths.
- **On the user's PC (Windows x64 CI artifact):**
  - The dock lists both RTX cards with the correct names, and "Encoder GPU = RTX 3050" really encodes on the 3050. Check that Task Manager's Video Encode graph rises on that card.
  - Projections are within roughly ±10 points of measured after calibration.
  - The warnings fire when too many sessions are set up on one GPU.
  - With the dock closed and nothing live, there is no measurable overhead.

# Status (handoff)
See [`HANDOFF.md`](../../HANDOFF.md) at the repository root for where work stopped and what to do next.

- Done: Part 0 (CI builds Windows x64 on pushes to `claude/**` in forks).
- Done but not yet built on Windows CI or tested on Windows: Part B (settings backup and restore). New files:
  - `frontend/utility/ZipArchive.*`
  - `frontend/utility/SettingsBackup.*`
  - `frontend/dialogs/OBSSettingsBackup.*`
  - `frontend/widgets/OBSBasic_Backup.cpp`
  - The restore hook is in `OBSApp::InitGlobalConfig`, the menu entries are set up in `OBSBasic::OBSInit`, and the auto backup runs in `OBSBasic::saveAll`.
  - The ZipArchive writer and reader were tested standalone on Linux. The rest only had a compile check against Qt 6.4 headers.
  - Deviation from the plan: the automatic-backup options are in the Back Up Settings dialog, not in Settings → General.
- Done but not yet built on Windows CI or tested on Windows: Part A phase 1 (vertical canvas core, settings dialog, Preview/Scenes/Sources docks, recording, replay buffer, multistream Output choice) plus the phase 3 hotkeys. HANDOFF.md lists the files and the deviations from this plan:
  - The settings are in their own Tools → Vertical Canvas dialog.
  - The canvas doesn't mix audio.
  - A new libobs function `obs_canvas_clear_video()` is used to turn vertical off.
- Not started: Part A phases 2–3 (except hotkeys) and Part C (hardware load projection).
