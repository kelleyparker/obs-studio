## OBS Studio with built-in multistreaming

Go live on YouTube, Twitch and Kick (or any RTMP/RTMPS server) at the same time with one **Start Streaming** click. Based on OBS Studio 33.0.0-rc1.

### Downloads

| File | For |
|---|---|
| `…-Windows-x64.zip` | Windows 10/11 and Windows Server (x64) |
| `…-macOS-Apple.dmg` | Macs with Apple Silicon |
| `…-Ubuntu-26.04-x86_64.deb` | Ubuntu 26.04 |
| `…-Sources.tar.*` | Source code |

### Setup

1. **Settings → Stream:** your main platform, as usual.
2. **Tools → Multistreaming:** turn it on, then add the other platforms with their stream keys.
   - **Same quality for every platform:** encodes once and sends the same stream everywhere.
   - **Separate stream per platform:** one encoder per platform. Use **Max** quality for each platform's own maximum (YouTube up to 4K60, Twitch 1080p60 at 6000 kbps, Kick 1080p at 8000 kbps), or pick a Twitch guide preset.
3. **Start Streaming.** The Multistream dock shows each platform's status, bitrate and dropped frames, with per-platform Start/Stop.

### Notes

- These builds are not code-signed.
  - **Windows:** if SmartScreen warns, click **More info → Run anyway**.
  - **macOS:** if macOS says the app is damaged or can't be verified, run `xattr -dr com.apple.quarantine /Applications/OBS.app`.
- **Windows zip:** it uses the same settings as an installed OBS. To keep them separate, put an empty file named `portable_mode.txt` in the unzipped folder, next to `bin`.
- **Ubuntu:** install with `sudo apt install ./OBS-Studio-*-Ubuntu-26.04-x86_64.deb`.

### Changes

- #1 frontend: Add built-in multistreaming
- #2 CI: Build only Windows x64, macOS arm64 and Ubuntu on this fork
