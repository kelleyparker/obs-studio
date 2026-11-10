# Handoff

Read this first if you are picking up work on this fork, whether you are a person or an AI
assistant. It covers:

- what the fork is for;
- how to build and test it;
- how the larger features are put together;
- what is left to do.

The README describes the features from a user's point of view.

## About the owner and the goal

- This is a personal fork of OBS Studio (`kelleyparker/obs-studio`). It has two aims:
  - build popular third-party plugin features into OBS, so fewer plugins are needed;
  - turn a webcam recording into a finished YouTube upload with as little hand work as possible.
- **Target: Windows x64 only.**
  - The macOS port was shelved on 2026-10-31, and macOS CI was dropped on 2026-11-10.
  - Linux, macOS and Windows on ARM aren't built or tested.
  - Windows-only APIs (DPAPI, PDH counters, conhost embedding, DXGI) are fine to use.
- **The owner's PC** is an X299 machine with two NVIDIA GPUs:
  - the **RTX 5070** (12 GB) drives the monitors;
  - the **RTX 3050** (8 GB) has no monitor attached. Use it for background GPU work so the
    desktop doesn't stutter.
- **Music stays AI-made:**
  - ACE-Step on this PC is the default.
  - Google Lyria (cloud, paid) is used only when the owner picks it, in Settings or per batch in
    Step mode.
  - Don't add stock or library music.

## Branches, CI and releases

- **Branches:**
  - All work is on `master`.
  - Do bigger changes on a `claude/**` branch and merge after the owner approves.
  - Commit or push only when the owner asks.
  - One old local branch is left: `claude/cranky-saha-2e4879`, with its worktree in
    `.claude/worktrees`. Its 2 commits are unmerged: an experimental per-step GPU choice, and music
    on both GPUs, from 2026-11-01. They're superseded by the music GPU picker, so they're kept only
    for reference.
- **CI** (`.github/workflows/push.yaml`, `pr-pull.yaml`):
  - Pushes to `master` run the format checks and validators only. They don't build.
  - Pushes to `claude/**` branches, tags and pull requests build **Windows x64** only.
  - A push that changes only `*.md` files starts nothing.
- **Releases:**
  - Pushing a `publish/**` branch runs `.github/workflows/fork-release.yaml`. It publishes the
    Windows build from a given build run as a prerelease. The run id, SHA and tag are set in that
    file.
  - The release notes are in `.github/fork-release/notes.md`.
  - The last release is `33.0.0-rc3-multistream2`. It predates the YouTube work.

## Building and running locally

The build uses Visual Studio 2026 Build Tools and CMake. The build folder is `build_x64`.

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

After the first configure, building just the frontend with MSBuild is faster:
`build_x64\frontend\obs-studio.vcxproj`, Configuration `RelWithDebInfo`.

- **Running:** run `build_x64\rundir\RelWithDebInfo\bin\64bit\obs64.exe`. Its working directory
  must be its own folder.
- **Python scripts:**
  - OBS reads them from `build_x64\rundir\RelWithDebInfo\data\obs-studio\scripts`.
  - The build copies them there (`frontend/cmake/ui-docks.cmake`).
  - To try a script change without rebuilding, copy the file there yourself.
- **Before you close or restart OBS:** check that it isn't recording or streaming, and close it
  gracefully by its PID, not with a kill.
- **Python tests** for the pipeline (`unittest`; some use real FFmpeg on short generated
  videos):

  ```powershell
  cd frontend\scripts
  python -m unittest test_youtube_steps test_youtube_broll test_youtube_retakes test_youtube_script
  ```

  `test_youtube_steps` alone has 137 tests and takes about 2 minutes.

## Code conventions that CI checks

- **Formatting:**
  - Format C and C++ with **clang-format 22** (22.1.3 in CI) using the repo's `.clang-format`.
  - CI fails on any difference.
  - To fix locally: `pip install clang-format==22.*`, then `clang-format -i <files>`.
- **C++17** (`cmake/common/compiler_common.cmake`):
  - Don't use designated initialisers or `char8_t`.
  - Take `QJsonArray` loop values by copy: `for (const QJsonValue v : array)`.
- **UI:** new frontend dialogs and docks are built in code, with no `.ui` forms.
- **Text:**
  - User-visible text for core OBS goes in `frontend/data/locale/en-US.ini` and is read with
    `QTStr("Key")`.
  - The YouTube dock and dialogs use plain English strings in code. The fork is English-only.
- **New files:** add new source files to the lists in `frontend/cmake/ui-*.cmake`. The YouTube
  files are in `ui-docks.cmake`.
- **Commits:** messages follow the OBS style, for example `frontend: Add X`, with a short body
  that says why.
- **Python** has no formatter in CI. Keep the style of the surrounding code.

## Where things stand

| Feature | State |
| --- | --- |
| Multistreaming (Settings → Stream toggle, Tools → Multistreaming) | Done, released in rc3 |
| Multistream chat docks, dashboard links, viewers/title/category | Done, released in rc3 |
| Fixed settings (audio, language, Advanced output, network, colors), removed services | Done |
| 50 themes and 50 styles, Cliffgoat Gold logo | Done |
| Settings backup and restore (File menu) | Done, builds on Windows; owner checklist below |
| Up to 8 canvases (Tools → Canvases) | Done, builds on Windows; owner checklist below |
| MKV recording, remuxed to MP4 | Done |
| System Monitor, Notepad and Music docks | Done |
| NVENC GPU names in the encoder settings | Done |
| Output Guide (Settings → Output) | Done |
| YouTube automation | In daily use by the owner; fixed as issues come up |
| Canvas editing in the preview (drag/resize), transitions, virtual camera | Not started |
| Projected hardware load per output, encoder GPU per destination | Not started (System Monitor shows live load) |

## YouTube automation

### How it runs

- **Arming:** the ▶ YT button in Controls arms the workflow.
- **When the recording stops** (and has been remuxed to MP4), `YouTubeWorkflowManager` starts
  the worker:
  - It runs `python youtube_pipeline.py --stdin-config`.
  - It writes one JSON object to the worker's stdin. That object holds the run's settings and
    the decrypted API keys.
  - The keys never touch disk or the environment.
  - The worker ignores `OPENAI_API_KEY`, `GEMINI_API_KEY`, `GOOGLE_API_KEY` and
    `ANTHROPIC_API_KEY`.
- **Events:** the worker sends JSON lines on stdout (`emit()` in the script). The common ones
  are:
  - `step`: drives the dock's progress rows.
  - `log`.
  - `review_ready` and `broll_plan_ready`: Step-mode pauses.
  - `metadata_ready`, `render_ready`, `thumbnail_ready`.
  - `needs_download`: asks before a big model download.
  - `complete`, `upload_pending`, `error`.

  `HandleStdoutLines` and `ProcessJsonEvent` in `YouTubeWorkflowManager.cpp` handle them.
- **Pausing at a gate:**
  - The worker writes `{stem}_yt_state.json` next to the recording. It holds the config without
    secrets, plus the job context.
  - The worker emits `review_ready` and exits.
  - OBS remembers the gate in `user.ini` (`[YouTubeWorkflow] PendingReview*`) and shows the
    review panel.
  - Approving starts the worker again with `mode: resume_pipeline` and the review. Then
    `run_stages` continues from `ctx["next_stage"]`.
- **Other worker modes:**
  - `redo_pipeline`: Start Over.
  - `thumbnail_regen`, `music_regen`, `broll_images`: regenerate at a gate.
  - `update_video`: Submit to YouTube.
  - `delete_video`, `oauth_login`, `export_premiere`.
  - `script`: the Script Writer, which has its own worker so it works while a job waits.
- **Stages, in order** (`run_stages`):

  ```
  cuts_review → broll_plan → render → metadata → music → thumbnails → publish → upload | finish
  ```

  Analysis, which is transcription and finding the cuts, runs first in `stage_analyze`.
- **Step and Auto mode:** Step mode sends `review_gates` (cuts, broll, metadata, music,
  thumbnail, publish). Auto sends none.
- **Skipping steps:**
  - The dock's 8 checkboxes become `skip_steps`, read through `skipped(config, step)`.
  - Music and B-roll are the same switches as in Settings: `music_enabled` and `broll.enabled`.
  - Resume, Continue and Start Over send the current list, so a paused job follows the boxes for
    the steps it hasn't reached.

### Files next to each recording

All are named after the recording, `{stem}_…`.

| File | What it is |
| --- | --- |
| `_yt_state.json` | The job: config (no secrets) and context. Open Project and Open Recent read these |
| `_yt_cuts.json`, `_yt_metadata.json`, `_yt_music.json`, `_yt_thumbs.json`, `_broll_plan.json` | What each review gate reads and writes back |
| `_yt_transcript.txt`, `_yt_titles.txt` | The transcript and the title options |
| `_yt_edited.mp4` | The edit (cuts, B-roll, loudness, fades) |
| `_yt_final.mp4` | The edit with music, which is uploaded |
| `_yt_thumb.jpg`, `_yt_thumb_bg.jpg` | The thumbnail, and the same picture without text |
| `_yt_thumbs\`, `_yt_refs\`, `_yt_music\`, `_broll\` | Candidates, reference images, songs, B-roll pictures |
| `_premiere\` | The Premiere Pro XML project and its media |

### Models and where they run

- **Transcription:** faster-whisper large-v3-turbo on CUDA, in the system Python.
- **Text** (title, description, B-roll plan): a ranked list of Gemini, OpenAI, Claude and local
  Ollama. A template is the fallback, and a template title is never uploaded as public.
- **Images:** local SDXL (`stabilityai/stable-diffusion-xl-base-1.0`), run by
  `youtube_imagegen.py`.
  - It uses its own venv in `%USERPROFILE%\OBSYouTubeImageGen`.
  - Reference images use IP-Adapter (`h94/IP-Adapter`). The ~3.2 GB download only happens after
    the owner agrees.
  - Gemini (Nano Banana) and GPT Image are the cloud options. Video frames and the owner's photo
    never leave the PC; reference images do, but only to the cloud model the owner picked.
- **Music:** ACE-Step 1.5, run by `youtube_musicgen.py`.
  - It uses its own venv in `%USERPROFILE%\OBSYouTubeMusicGen`.
  - It stays loaded between songs as a small local server (`server.json`: pid, port, token).
  - It runs below normal priority on the GPU picked in the music row: Auto picks a card of 6 GB
    or more with no monitor.
  - Ollama writes the song brief **on the same GPU**. It does this through a private
    `ollama serve` that sees only that card (`CUDA_VISIBLE_DEVICES` set to the card's UUID, and
    `OLLAMA_VULKAN=0`, otherwise Vulkan still shows it the other cards). The private server is
    stopped with its runner afterwards.
  - On the 3050 the brief uses llama3.1:8b, because gemma4:12b doesn't fit in 8 GB. It takes
    about 78 s there, against about 45 s on the 5070.
- **Script Writer:** Claude (Opus 5.5 at high effort by default). Scripts are saved to
  `%USERPROFILE%\OBSYouTubeScripts`.
- **API keys** (Gemini, OpenAI, Claude, YouTube token):
  - Stored with Windows DPAPI by `YouTubeSecretVault`, in Settings → YouTube Automation.
  - Read only in `LaunchPipelineWorker` and wiped after they're sent.

### Code map

| File | Purpose |
| --- | --- |
| `frontend/youtube/YouTubeWorkflowManager.*` | Config (`[YouTubeWorkflow]` in `user.ini`), starting and resuming the worker, events, gates, projects |
| `frontend/youtube/YouTubePipelineDock.*` | The dock: mode, models, the 8 step rows with checkboxes, review panels, title/thumbnail/upload area, console |
| `frontend/youtube/YouTubeCutEditor.*`, `YouTubeBrollReview.*`, `YouTubeMusicPicker.*`, `YouTubeThumbnailPicker.*` | The Step-mode review windows and panels |
| `frontend/youtube/YouTubeBrollRecorder.*` | Records a chosen screen source through its own `obs_view` while the main recording runs |
| `frontend/youtube/YouTubeScriptWriter.*`, `YouTubeTeleprompter.*` | Script Writer and teleprompter |
| `frontend/youtube/YouTubeSettingsWidget.*` | Settings → YouTube Automation |
| `frontend/youtube/YouTubeSecretVault.*` | DPAPI key storage |
| `frontend/youtube/YouTubeTerminalWidget.*` | The PowerShell console embedded beside the log (conhost, in a kill-on-close job object) |
| `frontend/youtube/YouTubeVideoPreview.*`, `YouTubeHeartbeat.*` | The edit preview, and the glow on the button that moves a Step job on |
| `frontend/scripts/youtube_pipeline.py` | The worker: every stage, the providers, upload |
| `frontend/scripts/youtube_imagegen.py`, `youtube_musicgen.py` | SDXL and ACE-Step, run in their own venvs |
| `frontend/scripts/premiere_export.py` | The Premiere Pro XML |
| `frontend/scripts/test_youtube_*.py` | Tests |

### Known gaps

- **B-roll planning:** the LLM still sees lines that were cut. It can plan a moment that is no
  longer in the edit; the guardrails then drop it.
- **Remux skipped:** when the Remux step is unchecked, the pipeline works on the MKV while OBS's
  own remux runs beside it.
- **Not tested end to end in OBS:** the step checkboxes. A real run with steps unchecked hasn't
  been done; the Python tests cover it.

## Other features

### Fixed settings

These are hidden in code, not removed from the `.ui` files, so merges from upstream stay simple.

- **Audio bitrate:** 320 kbps (`kFixedAudioBitrateKbps` in `frontend/utility/audio-encoders.hpp`).
  - Streams go lower where the platform needs it: through "Enforce streaming service bitrate
    limits", and through `CapAudioBitrate` in `MultistreamOutput.cpp`.
- **Audio format:** always 48 kHz stereo (`OBSBasic::ResetAudio`).
- **Accessibility:** the page and its color overrides are gone.
- **Network, Stream Delay and reconnect:** fixed at their defaults.
- **Simple output mode is gone:**
  - `OBSBasic::MigrateSimpleOutputMode` copies a Simple profile to Advanced once.
  - The Simple output code is still there but is never used.
- **Updates:** the section in Settings → General is hidden.

### Settings backup and restore

| File | Purpose |
| --- | --- |
| `frontend/utility/ZipArchive.*` | Minimal zip writer and reader (no compression; refuses unsafe paths) |
| `frontend/utility/SettingsBackup.*` | Backup, staging and applying a restore, automatic backups |
| `frontend/dialogs/OBSSettingsBackup.*` | The Back Up and Restore dialogs |
| `frontend/widgets/OBSBasic_Backup.cpp`, `frontend/OBSApp.cpp` (`ApplyPendingRestore`) | Menu glue, and applying a staged restore at the next start |

How a restore works:

- A restore first saves `obs-studio/backups/pre-restore-*.zip` and keeps the last 5.
- It then stages the files and applies them on the next start, before any config is loaded.
- If applying fails, it rolls back from that backup.

### Canvases

There are seven extra canvas slots next to the main one: Horizontal 2–4 and Vertical 1–4. Each
has its own scenes, docks, recording, replay buffer, hotkeys and settings (Tools → Canvases).

| File | Purpose |
| --- | --- |
| `frontend/utility/CanvasSlot.*`, `ExtraCanvas.*` | A canvas slot: enabled, resolution (profile), scenes, save and load with the scene collection |
| `frontend/utility/CanvasOutput.*` | Its recording and replay buffer |
| `frontend/dialogs/OBSCanvasSettings.*`, `frontend/docks/CanvasDocks.*` | Settings dialog; Preview, Scenes and Sources docks |
| `libobs/obs-canvas.c` | `obs_canvas_clear_video()`: drops a canvas's video mix but keeps its scenes |

How canvases behave:

- **Audio:** extra canvases don't mix audio (no `MIX_AUDIO`), so a microphone in two canvases
  isn't heard twice. Their outputs use the main audio mix.
- **Frame rate:** always the main canvas's.
- **Saved resolution:** `obs_save_canvas` doesn't save it, so the slot keeps it in the profile.
- **Scenes:** canvas scenes emit `source_create_canvas`, and `obs_get_source_by_name` and
  `obs_enum_scenes` only see the main canvas.
- **Multistream:** destinations can stream any enabled canvas. Extra canvases always get their
  own encoder.
- **Names:** Vertical 1 keeps every name the old single vertical canvas used.
- **Editing:** the preview can't be dragged or resized yet (see below).

### Multistream chat, dashboards and stream info

| File | Purpose |
| --- | --- |
| `frontend/utility/StreamPlatforms.*` | Channel normalizing; chat, dashboard and `/live` URLs |
| `frontend/utility/StreamInfo.*` | Platform credentials, live video ID, viewers, title, category |
| `frontend/docks/ChatDocks.*`, `frontend/dialogs/OBSPlatformAccounts.*` | Chat docks and the Platform Accounts dialog |

Two known limits:

- Posting in the YouTube chat dock may fail, because Google often blocks sign-in there.
- Kick may show a one-time Cloudflare check.

## Owner checklists

These came from the earlier work. Whether each has been run isn't recorded, so tick them off
here as they pass.

**Backup and restore:**

- [ ] **Full backup:** back up everything. The zip opens in Explorer and has `manifest.json`.
- [ ] **Without stream keys:** the keys in `profiles/*/service.json` are empty, and
      `[PlatformAccounts]` secrets are left out.
- [ ] **Replace:** delete a profile and a collection, then restore with **Replace** and restart.
      Both come back.
- [ ] **Merge:** restoring with **Merge** brings duplicates in as "… (Restored)".
- [ ] **Automatic backups:** turn them on, close OBS, and a new "OBS Auto Backup …" zip appears.

**Canvases:**

- [ ] **Turning a canvas on:** a canvas turned on in Tools → Canvases shows its three docks and a
      black preview.
- [ ] **Sources and scenes:**
  - Add a source and a main-canvas scene, then try Fill, Stretch, Center and Reset.
  - Restart OBS. The scenes, their order and the current scene come back.
- [ ] **Recording:** record a vertical canvas together with the main canvas. The file is
      1080x1920 at the main frame rate, with audio, and is remuxed to MP4.
- [ ] **Replay buffer:** start a canvas's replay buffer and save a replay with the button and
      with the hotkey.
- [ ] **Multistream:** a multistream destination on a vertical canvas streams 9:16 while the main
      stream is 16:9.
- [ ] **Turning off:** a canvas's settings can't be changed while it records. Switching scene
      collections while it records doesn't crash.

**Multistream chat:**

- [ ] **Dashboards:** each Dashboard button opens the right page. For YouTube that's the
      stream's Studio page once live.
- [ ] **Chat sign-in:** sign in to Twitch or Kick chat once, restart, and you're still signed in.
- [ ] **YouTube chat:** while live, the YouTube chat dock finds the live chat within about a
      minute.
- [ ] **Stream info:** while live, the Multistream dock shows viewers, title and category for
      each platform.

## What's next

Nothing is in progress. Ideas the owner has raised or that fell out of recent work:

1. **B-roll on the edited timeline:** give the B-roll planner only the lines that survive the
   cuts.
2. **Hardware load:**
   - projected load per output and per GPU, calibrated against measured load;
   - a stacked-bar load dock;
   - an encoder GPU choice per multistream destination and canvas.

   The System Monitor dock already reads the live counters (PDH) that this would build on.
3. **Canvas editing:**
   - Drag and resize in a canvas preview. This means making `OBSBasicPreview`, `SourceTree` and
     `OBSBasicTransform` work against a canvas other than the main one.
   - Then transitions, a canvas virtual camera, projectors and websocket support.
4. **A new fork release** that includes the YouTube work:
   - `.github/fork-release/notes.md` is already written for it.
   - Set the tag, title, SHA and build run id in `fork-release.yaml`. The run id must come from a
     `claude/**`, tag or PR build, since `master` pushes don't build.
   - Then push a `publish/**` branch.
