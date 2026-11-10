## OBS Studio with multistreaming, extra canvases and YouTube automation

This release adds four things to OBS:

- **Multistreaming:** go live on YouTube, Twitch and Kick (or any RTMP/RTMPS server) at once, with each platform's chat, dashboard and viewer count inside OBS.
- **Extra canvases:** record up to 8 video mixes, including vertical ones.
- **YouTube automation:** turn a webcam recording into an edited, titled, scored and thumbnailed YouTube upload.
- **Fewer settings:** settings that rarely change are fixed.

It is based on OBS Studio 33.0.0-rc3 and is for Windows x64 only.

### Downloads

| File | For |
|---|---|
| `…-Windows-x64.zip` | Windows 10/11 and Windows Server (x64) |

### What's new since 33.0.0-rc3-multistream2

- **YouTube automation:** press **▶ YT** next to **Start Recording**, record, and stop. The **YouTube Pipeline** dock then runs 8 steps:
  1. Remux to MP4.
  2. Transcribe with Whisper.
  3. Cut dead air and botched retakes.
  4. Add B-roll: AI pictures of what you say, and/or a screen recording.
  5. Write the title, description with chapters, and tags.
  6. Make background music.
  7. Make a thumbnail.
  8. Upload.

  How you control it:
  - **Step or Auto:** **Step** mode stops for your approval at each stage. **Auto** runs straight through, then uploads, exports a Premiere Pro project, or both.
  - **Skipping steps:** each step has a checkbox. Untick it to skip that step.
  - **Projects:** **New / Open / Open Recent Project** brings any job back where it was left, and **Start Over** redoes part of it.
  - **Scripts:** **Script…** writes video scripts with Claude, and a teleprompter scrolls them while you record.
  - **Settings:** everything is in **Settings → YouTube Automation**. API keys are stored only in OBS, encrypted with Windows DPAPI.
- **Up to 8 canvases** (**Tools → Canvases**): Horizontal 2–4 and Vertical 1–4 sit next to the main canvas.
  - Each one has its own scenes, sources, preview dock, recording, replay buffer and hotkeys.
  - A canvas can start and stop recording with the main recording.
  - Multistream destinations can stream any enabled canvas.
  - This replaces the single vertical canvas. Its scenes and settings carry over as **Vertical 1**.
- **MKV recording, remuxed to MP4:** every canvas records MKV by default. Each finished recording and replay buffer save is remuxed to an MP4 next to the MKV, which is kept.
- **New docks** (**Docks** menu):
  - **System Monitor:** CPU, memory, each GPU's load and VRAM, disks and network.
  - **Notepad.**
  - **Music:** YouTube Music in a browser dock.
- **Output Guide** in **Settings → Output:** reference notes for streaming, recording, audio, the replay buffer and platform limits.
- **Cliffgoat Gold theme** with the cliffgoat logo behind the window.

### Already in 33.0.0-rc3-multistream2

- **Multistreaming** (**Tools → Multistreaming**, or one click in **Settings → Stream**), with each Twitch, YouTube or Kick channel's chat dock, **Dashboard** button, and live viewer count, title and category.
- **Settings backup and restore** (**File → Back Up Settings / Restore Settings**).
- **50 themes and 50 styles** (**Settings → Appearance**).
- **GPU names for NVENC** in **Settings → Output**.
- **Fixed settings:**
  - audio is always 48 kHz stereo at 320 kbps, lowered per platform where needed;
  - English only;
  - Advanced output mode only;
  - fixed network, reconnect and stream delay settings.
- **Fewer services:** Twitch, YouTube (RTMPS and HLS), Facebook Live, Twitter, OnlyFans and Custom.

### Setup

1. **Settings → Stream:** your main platform, as usual.
2. **Tools → Multistreaming:** turn it on, then add the other platforms with their stream keys and channel names.
   - **Same quality for every platform:** encodes once and sends the same stream everywhere.
   - **Separate stream per platform:** one encoder per platform. Use **Max** quality for each platform's own maximum (YouTube up to 4K60, Twitch 1080p60 at 6000 kbps, Kick 1080p at 8000 kbps), or pick a Twitch guide preset.
3. **Start Streaming.** The Multistream dock shows each platform's status, bitrate, dropped frames and viewers, with per-platform Start/Stop.
4. **For the YouTube automation**, go to **Settings → YouTube Automation**:
   - sign in to YouTube with your own Google OAuth client;
   - add the API keys you want to use (Gemini, OpenAI, Claude);
   - choose the models.

   It also needs:
   - **Python 3** on `PATH`, with `faster-whisper`, `opencv-python`, `numpy`, `Pillow`, `google-genai`, `google-api-python-client` and `google-auth-oauthlib`;
   - **FFmpeg** on `PATH`;
   - an **NVIDIA GPU**.

   Optional local models:
   - **[Ollama](https://ollama.com):** titles written on your PC.
   - **Stable Diffusion XL:** thumbnails, in `%USERPROFILE%\OBSYouTubeImageGen`.
   - **ACE-Step 1.5:** music, in `%USERPROFILE%\OBSYouTubeMusicGen`.

   If a local model isn't installed, the log says how to install it.

### Notes

- **Pre-release:** the YouTube automation is new and has mostly been used on one PC.
- **YouTube chat:** Google often blocks sign-in inside OBS, so you may be able to read YouTube chat in the dock but not post in it. The YouTube chat dock shows a waiting message until the stream is live.
- **Code signing:** these builds are not code-signed. If SmartScreen warns, click **More info → Run anyway**.
- **Windows zip:** it uses the same settings as an installed OBS. To keep them separate, put an empty file named `portable_mode.txt` in the unzipped folder, next to `bin`.
