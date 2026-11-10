OBS Studio, cliffgoat fork
==========================

This is a personal fork of `OBS Studio <https://obsproject.com>`_ (``kelleyparker/obs-studio``).

It does three things:

- It builds popular plugin features into OBS, so fewer plugins are needed.
- It turns a webcam recording into a finished YouTube upload: the edit, title, music, thumbnail
  and upload.
- It removes settings its owner never changes.

It targets **Windows x64 only**. The macOS port was shelved on 2026-10-31. Linux, macOS and
Windows on ARM are neither built nor tested, and several features below use Windows-only APIs.

.. contents:: On this page
   :local:
   :depth: 1

YouTube automation
------------------

Turn it on with the **▶ YT** button in Controls, then record as usual. When the recording stops,
the **YouTube Pipeline** dock takes the video through 8 steps:

1. Remux the MKV recording to MP4.
2. Transcribe it with Whisper (faster-whisper, large-v3-turbo, on the GPU).
3. Cut dead air and botched retakes.
4. Plan B-roll.
5. Write the title, 9 alternative titles, the description with chapters, and tags.
6. Make background music.
7. Make a 1280×720 thumbnail.
8. Upload to YouTube, unlisted by default.

Each step has a checkbox. Untick a step and its row greys out, and the next run skips it (and,
in Step mode, its review).

Modes
^^^^^

- **Step mode** pauses for you at the cuts, the B-roll, the title and description, the music,
  the thumbnail, and finally at **Ready to publish**.
- **Auto mode** runs straight through. When it finishes, it uploads, exports for Premiere Pro,
  or both.

Editing
^^^^^^^

- **Cut editor:** a scrubber, a timeline and a dial for each cut, or for all of them at once.
- **Fades:** every edit fades in from black and out to black.
- **Loudness:** the voice is brought to YouTube's −14 LUFS, with a limiter so it can't clip.
- **Premiere Pro export:** an XML project with the cuts, B-roll on V2, dissolves and the music,
  opened straight in Premiere.

B-roll
^^^^^^

- **AI pictures** of what you say, shown with a slow zoom. A 0–5 dial sets how many.
- **Screen B-roll:** a screen source recorded alongside the webcam.
- The **YouTube: Mark B-roll moment** hotkey marks moments to use while you record.
- Each insert can be reviewed, nudged, re-prompted, and shown as picture-in-picture or a
  cutaway.

Titles and descriptions
^^^^^^^^^^^^^^^^^^^^^^^

These come from a ranked list of providers: Gemini, OpenAI, Claude and a local Ollama model.
If none of them answers, a template is used, and the video is never published as public with a
template title.

Music
^^^^^

- **ACE-Step 1.5** makes the songs on your own GPU. Ollama writes the brief for each song.
- **Google Lyria** is an optional cloud source, chosen per batch.
- The music step can run on a spare GPU with no monitor attached, so the desktop doesn't
  stutter. ACE-Step stays loaded between songs.

Thumbnails
^^^^^^^^^^

- **Models:** local SDXL is the default. Gemini (Nano Banana), GPT Image or Ollama can be used
  instead.
- **In the picker:**

  - your own prompt;
  - up to 4 reference images (local SDXL uses them through IP-Adapter);
  - img2img refinement;
  - a cut-out of you, from a photo or the video.

- **After the run:** edit the thumbnail in any image editor and **Submit to YouTube** replaces
  it on the upload.

Projects
^^^^^^^^

- **New Project, Open Project and Open Recent** bring any job back exactly where it was left.
- **Start Over** redoes the cuts, title, music or thumbnail. It can delete the YouTube upload
  first.

Scripts and teleprompter
^^^^^^^^^^^^^^^^^^^^^^^^

- **Script…** opens a Script Writer: Claude suggests topics and writes or revises the spoken
  script.
- The **teleprompter** scrolls the script at a set number of words per minute. It stays out of
  screen recordings and can start with the recording.

Other dock features
^^^^^^^^^^^^^^^^^^^

- A workflow console log, with a PowerShell console embedded beside it.

Settings, keys and requirements
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

- **Settings → YouTube Automation** holds:

  - the provider ranking and models;
  - the YouTube sign-in and upload defaults;
  - the editing presets;
  - the B-roll and music options.

- **API keys** are stored only in OBS, encrypted with Windows DPAPI. Environment variables are
  ignored.
- **The pipeline needs:**

  - Python 3 and FFmpeg on ``PATH``;
  - an NVIDIA GPU;
  - for the local LLM steps, `Ollama <https://ollama.com>`_.

- **Local image and music models** live in their own Python environments,
  ``%USERPROFILE%\OBSYouTubeImageGen`` and ``%USERPROFILE%\OBSYouTubeMusicGen``.
- **Where the code is:**

  - the worker scripts are in ``frontend/scripts`` (``youtube_pipeline.py``, ``youtube_imagegen.py``,
    ``youtube_musicgen.py``, ``premiere_export.py``);
  - the dock and dialogs are in ``frontend/youtube``.

Other additions
---------------

- **Multistreaming:**

  - Stream to several platforms at once, alongside the main stream.
  - Turn it on with one click in Settings → Stream, or set up destinations in
    Tools → Multistreaming.
  - Destinations can share the main encoder or get their own, can use each platform's maximum
    quality, and can stream any enabled canvas.

- **Multistream chat, dashboards and stream info:**

  - Give each Twitch, YouTube or Kick destination (and the main stream) its channel name. Each
    channel then gets a chat dock and a Dashboard button.
  - With free developer credentials, the Multistream dock also shows each channel's live viewer
    count, title and category.

- **Up to 8 canvases** (Tools → Canvases, like Aitum Vertical):

  - Horizontal 2–4 and Vertical 1–4 sit next to the main canvas.
  - Each one has its own scenes, sources, preview dock, recording, replay buffer and hotkeys.
  - A canvas can start and stop recording with the main recording.

- **MKV recording, remuxed to MP4:**

  - Every canvas records MKV by default.
  - Each finished recording and replay buffer save is remuxed to an MP4 next to the MKV, which is
    kept.

- **Settings backup and restore** (File → Back Up Settings / Restore Settings):

  - Saves profiles, scene collections, app settings and plugin settings in one ``.zip``.
  - Can also back up automatically every time OBS closes.

- **Docks:**

  - **System Monitor:** CPU, memory, each GPU's load and VRAM, disks and network, with a
    one-minute history.
  - **Notepad:** saves itself, and can draft a sample script with the local Ollama model.
  - **Music:** a YouTube Music browser dock.

- **NVENC GPU names:** the encoder's GPU setting lists each NVIDIA card by name instead of an
  index.

- **Output Guide** in Settings → Output: reference notes for streaming, recording, audio, the
  replay buffer and Twitch, YouTube and Kick limits.

- **Themes:**

  - 50 themes (Yami plus dark and light themes such as Nord, Dracula, Tokyo Night and Solarized)
    and 50 accent styles. They are generated by ``frontend/tools/generate_themes.py``.
  - The Cliffgoat Gold theme draws the cliffgoat logo behind the window.

Fixed settings
--------------

These are no longer settings. OBS always uses:

- **Audio:** 48 kHz stereo, encoded at 320 kbps. Each stream is lowered to its platform's maximum
  where the platform accepts less (for example 160 kbps for YouTube).
- **Language:** English.
- **Output mode:** Advanced. Profiles that used Simple mode are moved to Advanced, keeping their
  encoder, bitrate and recording settings.
- **Network:**

  - IPv4 and IPv6, the default network adapter, and no dynamic bitrate.
  - Streams reconnect automatically, every 2 seconds and up to 25 times, and never use a stream
    delay.

- **Colors:** the theme's own preview and audio meter colors.

Removed
-------

- **Settings pages and sections:**

  - the Accessibility page and the Language setting;
  - Audio → General (sample rate and channels) and every audio bitrate setting;
  - the Output Mode choice;
  - the Network, Stream Delay and Automatically Reconnect sections of Settings → Advanced;
  - the Updates section.

- **Streaming services:**

  - Every service except Twitch, YouTube (RTMPS and HLS), Facebook Live, Twitter, OnlyFans and
    Custom.
  - WHIP and "Show All" are gone, Twitch and Twitter list only their US servers, and the service
    list is no longer updated online.

Building
--------

The fork is built with CMake and Visual Studio 2026 (Build Tools are enough):

.. code-block:: powershell

   cmake --preset windows-x64
   cmake --build --preset windows-x64 --config RelWithDebInfo

The build runs from ``build_x64\rundir\RelWithDebInfo\bin\64bit\obs64.exe``. Python scripts are
read from ``build_x64\rundir\RelWithDebInfo\data\obs-studio\scripts``. After editing a script in
``frontend/scripts``, copy it there; a rebuild does the same.

To run the pipeline's tests:

.. code-block:: powershell

   cd frontend\scripts
   python -m unittest test_youtube_steps test_youtube_broll test_youtube_retakes test_youtube_script

For general OBS build help, see the upstream
`build instructions <https://github.com/obsproject/obs-studio/wiki/Install-Instructions>`_.
``HANDOFF.md`` has notes on the earlier canvas, backup and multistream work.

What is OBS Studio?
-------------------

OBS Studio is software for capturing, compositing, encoding, recording and streaming video. It is
distributed under the GNU General Public License v2 (or any later version); see ``COPYING``.

This fork is not affiliated with the OBS Project. For OBS itself:

- Website: https://obsproject.com
- Help and guides: https://github.com/obsproject/obs-studio/wiki
- Forums: https://obsproject.com/forum/
- Developer and API documentation: https://obsproject.com/docs
- Upstream bug tracker: https://github.com/obsproject/obs-studio/issues
- Contributing to OBS: `CONTRIBUTING.md <https://github.com/obsproject/obs-studio/blob/master/CONTRIBUTING.md>`_,
  `CODESTYLE.md <https://github.com/obsproject/obs-studio/blob/master/CODESTYLE.md>`_ and the
  `Code of Conduct <https://github.com/obsproject/obs-studio/blob/master/COC.rst>`_
- Supporting the project: https://obsproject.com/contribute
