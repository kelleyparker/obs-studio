## OBS Studio with built-in multistreaming and chat

Go live on YouTube, Twitch and Kick (or any RTMP/RTMPS server) at the same time with one **Start Streaming** click, with each platform's chat, dashboard and viewer count inside OBS. Based on OBS Studio 33.0.0-rc3.

### Downloads

| File | For |
|---|---|
| `…-Windows-x64.zip` | Windows 10/11 and Windows Server (x64) |
| `…-macOS-Apple.dmg` | Macs with Apple Silicon |

### What's new since 33.0.0-rc1-multistream1

- **Chat, dashboards and stream info:** give each Twitch, YouTube or Kick channel its name in **Tools → Multistreaming**. Each channel then gets:
  - a chat dock (**Docks** menu), where you sign in once;
  - a **Dashboard** button that opens the platform's stream dashboard in your browser;
  - its live viewer count, title and category in the Multistream dock, once you add free developer credentials in **Tools → Multistreaming → Platform Accounts**.
- **Vertical canvas** (**Tools → Vertical Canvas**): a second 9:16 video mix with its own scenes and sources, a preview dock, recording, replay buffer and hotkeys. Multistream destinations can stream it.
- **Settings backup and restore** (**File → Back Up Settings / Restore Settings**): profiles, scene collections, app and plugin settings in one `.zip`, with optional automatic backups when OBS closes.
- **50 themes and 50 styles** (**Settings → Appearance**).
- **Multistreaming toggle** in **Settings → Stream**.
- **Fewer settings:** audio is always 48 kHz stereo at 320 kbps (lowered per platform where needed), English only, Advanced output mode only, and fixed network, reconnect and stream delay settings.
- **Fewer services:** Twitch, YouTube (RTMPS and HLS), Facebook Live, Twitter, OnlyFans and Custom.
- Updated to upstream OBS Studio 33.0.0-rc3.

### Setup

1. **Settings → Stream:** your main platform, as usual.
2. **Tools → Multistreaming:** turn it on, then add the other platforms with their stream keys and channel names.
   - **Same quality for every platform:** encodes once and sends the same stream everywhere.
   - **Separate stream per platform:** one encoder per platform. Use **Max** quality for each platform's own maximum (YouTube up to 4K60, Twitch 1080p60 at 6000 kbps, Kick 1080p at 8000 kbps), or pick a Twitch guide preset.
3. **Start Streaming.** The Multistream dock shows each platform's status, bitrate, dropped frames and viewers, with per-platform Start/Stop.

### Notes

- This is a pre-release. The chat docks, dashboard links and stream info are new and haven't been fully tested yet.
- **YouTube chat:** Google often blocks sign-in inside OBS, so you may be able to read YouTube chat in the dock but not post in it. The YouTube chat dock shows a waiting message until the stream is live.
- These builds are not code-signed.
  - **Windows:** if SmartScreen warns, click **More info → Run anyway**.
  - **macOS:** if macOS says the app is damaged or can't be verified, run `xattr -dr com.apple.quarantine /Applications/OBS.app`.
- **Windows zip:** it uses the same settings as an installed OBS. To keep them separate, put an empty file named `portable_mode.txt` in the unzipped folder, next to `bin`.
