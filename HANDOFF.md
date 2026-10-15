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
- **Only Windows x64 matters for now.** The owner uses a Windows PC: an X299 machine with two
  NVIDIA GPUs, an RTX 5070 and an RTX 3050. Other platforms are not built or tested, but the code
  should stay portable (plain Qt and libobs) where possible.
- The owner tests the Windows x64 build that CI produces. Nobody builds locally.

## Where work stopped

| Part | What it is | State |
| --- | --- | --- |
| 0 | CI builds Windows x64 on pushes to `claude/**` branches of the fork | Done |
| B | One-click settings backup and restore (File menu) | **Code done. Not yet compiled on Windows CI and not tested on Windows** |
| A | Built-in vertical canvas (like Aitum Vertical) | Not started |
| C | Projected hardware load per output, per GPU; encoder GPU choice per destination | Not started |

Order of work: 0 → B → A phase 1 → C → A phase 2 → A phase 3. Each step should end with a green
Windows x64 CI build that the owner has tested.

### Next steps, in order

1. **Turn GitHub Actions back on.** As of 2026-10-15, Actions is disabled for this repository, so
   nothing builds. Turn it on in **Settings → Actions → General** ("Allow all actions and reusable
   workflows").
2. **Get Part B building on Windows CI.** Open the **Actions** tab, then **Push**, then
   **Run workflow**, and pick this branch. Fix any MSVC errors. Part B has only been compiled
   against Qt 6.4 on Linux, so expect a few small MSVC or Qt 6.8 fixes.
3. **The owner tests Part B** with the CI artifact `obs-studio-windows-x64-*`. Use the checklist
   under "Testing Part B" below.
4. **Start Part A, phase 1a:** the `VerticalCanvas` manager. See "Starting Part A" below and the plan.

## Branches and workflow

- **Development branch:** `claude/wonderful-hopper-3mrp6t`, which is 3 commits ahead of `master`.
  Merge it to `master` only after the owner approves.
- **CI:**
  - Pushes to `claude/**` build Windows x64 only (`.github/workflows/push.yaml`). The workflow can
    also be started by hand with **Run workflow**.
  - A push that changes only `*.md` files does not start a build. For a new branch, GitHub only
    looks at the newest commit when it decides this.
  - The artifact to download is `obs-studio-windows-x64-*`.
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

## Starting Part A (vertical canvas)

Phase 1 has five sub-steps (1a–1e). The plan describes them in detail. The libobs pieces already
exist, but the frontend doesn't use them yet:

- **Canvas API:** `libobs/obs.h` (`obs_canvas_*`) and `libobs/obs-canvas.c`. Create the canvas with
  `OBS_CANVAS_ACTIVATE | OBS_CANVAS_MIX_AUDIO | OBS_CANVAS_SCENE_REF`.
- **Frontend canvas helpers:** `OBSBasic::AddCanvas/RemoveCanvas` in
  `frontend/widgets/OBSBasic_Canvases.cpp`, and the `OBS::Canvas` wrapper in
  `frontend/utility/OBSCanvas.*`. Canvases are saved in the scene collection under `"canvases"`
  (`frontend/widgets/OBSBasic_SceneCollections.cpp`).
- **Binding an encoder to a canvas:**
  `obs_encoder_set_video(enc, obs_canvas_get_video(canvas))`. See
  `frontend/utility/MultitrackVideoOutput.cpp`.
- **Things to watch out for:**
  - `obs_save_canvas` doesn't save the resolution, so store it and call `obs_canvas_reset_video`
    after loading.
  - The canvas frame rate always follows the main canvas.
  - `obs_get_source_by_name` and `obs_enum_scenes` only see the main canvas.
  - Check early whether a source that is in both canvases has its audio mixed twice.
- **Multistream** (to stream the vertical canvas):
  - `frontend/utility/MultistreamOutput.*`, `frontend/dialogs/OBSMultistreamSettings.*` and
    `frontend/docks/MultistreamDock.*`.
  - Each destination gets a `canvas` field (`"main"` or `"vertical"`) in `multistream.json`.
  - Vertical destinations always use their own encoder.

## Part C (hardware load)

Not started. The plan covers it in full:

- live GPU, CPU, network and disk counters through Windows PDH;
- NVIDIA details through `nvml.dll`, loaded at runtime;
- matching NVENC device numbers to DXGI adapters by PCI bus id;
- per-output load estimates that are calibrated against measured load;
- a Hardware Load dock with one stacked bar per component;
- an "Encoder GPU" choice for each multistream destination and for vertical recording.
