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
| D | Multistream chat docks, dashboard links, live viewers/title/category (branch `claude/multistream-chat`) | **Code done. Not compiled anywhere yet, not tested** |

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

- **Branches:** as of 2026-10-15 all work is on `master`, the only branch. The owner approved
  merging `claude/wonderful-hopper-3mrp6t`, and it and the older branches were deleted. Do new
  work on a `claude/**` branch and merge it to `master` only after the owner approves.
- **CI:**
  - Pushes to `master` and `claude/**`, and pull requests, build Windows x64 and macOS arm64
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

## Audio bitrate fixed at 320 kbps (done, untested)

At the owner's request, audio is always encoded at 320 kbps and there is no audio bitrate setting
(`kFixedAudioBitrateKbps` in `frontend/utility/audio-encoders.hpp`). This covers Simple and
Advanced output, custom FFmpeg output, vertical recording and multistream. The bitrate rows are
hidden in code (`OBSBasicSettings::LoadOutputSettings`), not removed from the `.ui` file, so
merges from upstream stay simple. Saved `ABitrate`/`TrackNBitrate` values are ignored.

Streams still go lower where the platform accepts less: the main stream through "Enforce
streaming service bitrate limits", and every multistream destination (shared or separate
encoders) through `CapAudioBitrate` in `MultistreamOutput.cpp`, for example 160 kbps for YouTube
and 128 kbps for Facebook.

## Audio fixed at 48 kHz stereo, Accessibility page hidden (done, untested)

At the owner's request:

- Audio is always 48 kHz stereo (`OBSBasic::ResetAudio`). Settings → Audio → General is hidden,
  saved `SampleRate` and `ChannelSetup` values are ignored, and switching profiles never asks for
  a restart because of audio.
- Settings → Accessibility is hidden and its color overrides are ignored. The preview borders and
  the volume meters always use the default (theme) colors.

## Settings fixed at defaults, Simple output mode removed (done, untested on Windows)

At the owner's request, these are fixed in code and hidden in Settings (hidden from code, not
removed from the `.ui` file, to keep upstream merges simple):

- Settings → Advanced → Network: IPv4 and IPv6, default bind IP, no dynamic bitrate, and on
  Windows the default socket loop without low latency mode.
- Settings → Advanced → Stream Delay (always off) and Automatically Reconnect (always on, 2 s,
  25 retries). This covers multistream destinations too.
- **Simple output mode is gone.** The Output page always shows Advanced and the Output Mode
  choice is hidden. `OBSBasic::MigrateSimpleOutputMode` (run from `UpdateProfileEncoders`, before
  the outputs are created) copies a Simple profile's encoder, bitrate, preset, recording path,
  format, tracks and replay buffer to Advanced once, writes `streamEncoder.json`, and sets
  `Mode=Advanced`. New profiles start from the Simple defaults and are migrated the same way.
  Simple recording qualities other than "Same as stream" become the chosen encoder at its default
  settings; "Lossless" becomes "Same as stream". The Auto-Configuration Wizard writes Advanced
  settings. The Simple output code is still there but is never used.

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

## Part D: multistream chat, dashboards and stream info (done, not compiled)

On branch `claude/multistream-chat`, not merged. Nothing here has been compiled yet. CI and
local builds wait for the owner's go-ahead, so expect a few small compile fixes on the first
build.

- **Channels:** each Twitch, YouTube or Kick destination has a **Channel** field (Tools →
  Multistreaming → Edit). The main stream gets one under "Main stream channel", where Platform
  is Auto (detected from Settings → Stream) or Twitch, YouTube or Kick. A pasted channel URL
  works too. It's stored in `multistream.json` as `channel`, `primary_platform` and
  `primary_channel`.
- **Chat docks:** one dock per channel, named "<name> Chat" in the Docks menu and hidden until
  shown.
  - It loads the platform's pop-out chat and uses the custom browser docks' cookie store, so a
    sign-in survives restarts and profile switches.
  - The header has **Dashboard**, **Open in Browser** and **Reload** buttons.
  - YouTube's chat needs the live video ID. It's found by reading the channel's `/live` page
    (canonical `watch?v=` link) once the channel is live, then every minute until found. With
    an API key, the API is the fallback.
  - Docks created after start-up get their saved place back through
    `OBSBasic::AddDockWidget(..., restore = true)`, which calls `restoreDockWidget`.
- **Dashboard links** open in the system browser, because Google blocks sign-in inside
  embedded browsers:
  - Twitch: `dashboard.twitch.tv/u/<login>/stream-manager`
  - YouTube: `studio.youtube.com/video/<id>/livestreaming` once live, otherwise
    `youtube.com/live_dashboard`
  - Kick: `dashboard.kick.com/stream`
- **Stream info:** while a channel is live, `StreamInfoFetcher` polls every 60 s on its own
  one-thread pool using `GetRemoteFile`.
  - The Multistream dock shows the viewers per channel, a line with the title and category, and
    a total.
  - Twitch uses Helix `streams` with an app token, Kick `public/v1/channels?slug=` with an app
    token, and YouTube `videos?part=snippet,liveStreamingDetails` with an API key (about 60
    quota units an hour).
- **Credentials:** Tools → Multistreaming → **Platform Accounts…**, saved in `user.ini`
  `[PlatformAccounts]`. Each platform has a Test button. A backup made without stream keys also
  drops the secrets.

| File | Purpose |
| --- | --- |
| `frontend/utility/StreamPlatforms.{hpp,cpp}` | Platform ids, channel normalizing, chat, dashboard and `/live` URLs, main-stream platform detection, list of channels |
| `frontend/utility/StreamInfo.{hpp,cpp}` | `PlatformCredentials`, `TestPlatformCredentials`, `StreamInfoFetcher` (YouTube live video ID, viewers, title, category) |
| `frontend/docks/ChatDocks.{hpp,cpp}` | `MultistreamChatDocks`, only built with browser panels |
| `frontend/dialogs/OBSPlatformAccounts.{hpp,cpp}` | The Platform Accounts dialog |
| `frontend/docks/MultistreamDock.*`, `frontend/dialogs/OBSMultistreamSettings.*`, `frontend/utility/MultistreamOutput.*` | Channel fields, Dashboard buttons, viewers, title and category |
| `frontend/widgets/OBSBasic*.cpp` | Creation (before `restoreState`), teardown, lock and reset of docks, refresh after Settings |

Known limits: posting in the YouTube chat dock may fail, because Google often refuses sign-in
there. Use **Open in Browser** instead. Kick's dock may show a one-time Cloudflare check.

### Testing Part D (owner, once built)

- [ ] Set channels for the main stream and the destinations. The Docks menu lists "<name> Chat"
      for each.
- [ ] Each Dashboard button opens the right page in the web browser. For YouTube, that's the
      stream's Studio page once live.
- [ ] Twitch and Kick chat load while offline. Sign in once, restart OBS, and you're still
      signed in.
- [ ] Go live: the YouTube chat dock switches from "waiting" to the live chat within about a
      minute.
- [ ] Dock positions and visibility survive a restart and a profile switch. Removing a
      destination removes its dock. Lock Docks and Reset UI include the chat docks.
- [ ] Platform Accounts → Test passes with valid credentials and shows a clear error with a
      wrong secret.
- [ ] While live, the Multistream dock shows viewers, title and category for each platform
      within about a minute, plus a total. Without credentials it shows "—" and nothing breaks.
- [ ] A backup made without stream keys has no `[PlatformAccounts]` secrets in `user.ini`.

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
