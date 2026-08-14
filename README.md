<p align="center">
  <img src="assets/brand/BENCO_Logo_README.png" alt="BENCO Holdings" width="420">
</p>

# BENCsnip

A video editor for the jobs that should take a minute. Drag a file in, cut the
boring bit out, export. It opens whatever ffmpeg opens, which is nearly
everything, and it does not ask you to make an account first.

![the BENCsnip window](docs/gui.png)

It is roughly what Clipchamp is for, without the sign-in, the upload, the
watermark, the subscription tier that unlocks 1080p, or the four seconds of
splash screen. One executable, no installer, no runtime, no project cloud.

**One binary.** The font, the licences and the mascot are compiled into it —
there is no assets folder to lose. Link a static ffmpeg (see below) and the
result runs on a machine that has never heard of ffmpeg either.

---

## Quick start

```
make
./bencsnip
```

Then drag a video onto the window. Or start with one:

```
./bencsnip holiday.mp4
```

To cut a piece out of the middle: put the playhead where the boring bit starts,
press **S**, put it where the boring bit ends, press **S** again, click the
middle piece and press **Shift+Delete**. Then **Ctrl+E**.

---

## What it does

* **Anything in.** MP4, MOV, MKV, WebM, AVI, MPEG-TS, phone video, screen
  recordings, MP3, WAV, FLAC, Opus — if libav can demux it, it lands on the
  timeline. Portrait video from a phone comes in the right way up, because the
  display matrix is read rather than ignored.
* **A timeline that behaves.** Drag clips to move them, drag their edges to
  trim, drag the corner handles to fade. Snapping to cuts and to the playhead,
  which can be turned off. Video and its audio are linked and move together
  until you split them apart.
* **A preview that stays in sync.** The picture is chosen to match how much
  sound the audio device has actually played, so a slow decode drops a frame
  rather than sliding the sound away from the picture.
* **Export that is honest about what it is doing.** The dialog tells you
  whether it is going to re-encode the whole timeline or copy the packets
  across untouched — and it copies whenever it can.
* **Undo everything**, two hundred steps deep.

### The fast trim

Trimming one file and exporting it in the same format does not need to
re-encode anything. BENCsnip notices, copies the packets straight across, and a
four-gigabyte camera file is trimmed in about a second with no quality lost at
all. The export dialog says so before you commit:

```
fast trim: the file is copied, not re-encoded.
it starts at the keyframe before the in point.
```

That second line is the deal you are making. A copy can only begin at a
keyframe, so the result may include up to a GOP — usually under two seconds —
before the in point you set. When that matters, change any export setting (the
size, the frame rate, the format) and it renders instead, exactly.

Anything else — two files, a cut, a fade, a resize — renders. Rendering is
exactly what the preview showed you, because both go through the same code.

---

## Keys

| | |
|---|---|
| **Space** | play / pause |
| **S** | split every track at the playhead |
| **Delete** | delete the selected clip |
| **Shift+Delete** | delete it and close the gap |
| **M** | mute the selected clip |
| **← →** | one frame; hold Shift for a second |
| **, .** | to the previous / next cut |
| **Home / End** | to the beginning / the end |
| **F** | fit the whole timeline in the window |
| **+ −** | zoom in / out |
| **Ctrl+Z / Ctrl+Shift+Z** | undo / redo |
| **Ctrl+I** | add media to the bin |
| **Ctrl+S / Ctrl+Shift+S** | save the project / save it somewhere else |
| **Ctrl+O** | open a project |
| **Ctrl+E** | export |
| **Ctrl+N** | start again |
| **Ctrl+A** | select everything |
| **Esc** | select nothing |
| **F12** | write a screenshot beside the program |

The mouse: click a clip to select it, drag the middle to move it, drag either
edge to trim it, drag the small square in a top corner to make a fade. The
ruler scrubs. The wheel scrolls; Ctrl or Shift with it zooms; the middle
button pans. Right-click a clip for the rest.

---

## Building

A GNU makefile, a C++17 compiler, raylib and the ffmpeg libraries. No CMake
step, no package manager, no code generation beyond turning the font into a C
array.

### Linux

```
sudo pacman -S base-devel ffmpeg raylib        # Arch
sudo apt install build-essential libavformat-dev libavcodec-dev \
                 libswscale-dev libswresample-dev libraylib-dev   # Debian/Ubuntu

make
```

If your distribution has no raylib package, drop a prefix in `vendor/raylib`
with `include/` and `lib/libraylib.a` in it, or pass `RAYLIB=/somewhere`. The
build prefers a static `libraylib.a` where it finds one, so the result runs on
a machine that has never heard of raylib.

### macOS

```
brew install ffmpeg raylib
make
```

### Windows

MSYS2, in the MINGW64 shell:

```
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-ffmpeg mingw-w64-x86_64-raylib
make
```

The Windows link is `-static`: without it the executable imports
`libstdc++-6.dll`, `libgcc_s_seh-1.dll` and `libwinpthread-1.dll`, which live
inside an MSYS2 installation and nowhere else — it runs on the machine that
built it and fails to start everywhere else, with a dialog naming a file the
person has never heard of.

### A self-contained binary

By default the ffmpeg libraries are linked as shared objects, which is the
right choice for a package: the user owns those libraries and can replace them,
which is also what the LGPL asks for. For a binary you hand to someone
directly, build a static ffmpeg prefix into `vendor/ffmpeg` (`include/`,
`lib/*.a` and `lib/pkgconfig/`) and `make` picks it up instead — see NOTICE for
what shipping that obliges you to do.

```
make info      # what the build decided about raylib and ffmpeg, and from where
```

### Other targets

```
make test        the core tests: the timeline, the decoder, the mixer, both
                 export paths. No window and no sound card needed.
make testmedia   writes three small test files into media/ with ffmpeg
make probe       a command-line media probe, for when it is unclear whether a
                 problem is the decoder or the interface
make icons       regenerates assets/icon from the star the program draws
make clean
```

---

## Project files

A `.bencsnip` is lines of text, and it holds no media — a clip is a path, an in
point and an out point. Paths inside the project's own folder are stored
relative to it, so the folder can be moved or copied to another machine and
still open.

```
bencsnip 1
name holiday
video 1920 1080 30.000000
item 3 128.400000 1 1 clips/MVI_0043.MP4
track 5 0 0 0 0 V1
clip 8 3 9 12.000000 46.500000 0.000000 1.0000 0.0000 0.5000 0
```

It is a text file on purpose. The one time you need to look at a project file
is when something has gone wrong — a moved drive, a renamed folder — and a file
you can open in an editor and fix is worth more than one you cannot. A file
that has gone missing is kept in the bin, marked, with the duration the project
remembered; the edit survives and relinking one path brings it back.

---

## How it is put together

```
src/core/     sn_media       libav: probe, decode, seek, resample
              sn_timeline    tracks, clips, and every edit
              sn_render      the timeline as pictures and sound
              sn_project     save and load
              sn_export      the fast path and the rendering path

src/gui/      sn_gui         theme and widgets, drawn not toolkitted
              sn_player      the decode thread and the audio clock
              sn_bin         the media pane, and its thumbnail worker
              sn_track       the timeline pane
              sn_dialog      export, information, the file browser
              main.cpp       layout, keyboard, and the glue
```

`src/core` does not include raylib and never opens a window. That is what makes
`make test` possible on a machine with no screen and no sound card, and it is
much cheaper to hold to from the start than to retrofit.

Two decisions do most of the work:

**One renderer, two callers.** The preview and the exporter both ask
`sn::Renderer` the same question — what does the timeline look and sound like
at time *t* — so what you watched is what gets written. The usual way that goes
wrong is two pieces of code that both know how fades work.

**The audio device is the clock.** The worker thread mixes ahead into a ring
buffer; the device drains it; how much it has drained is the playback position,
and the picture is chosen to match. A preview driven by the wall clock or by
frames drawn slides out of sync with its own sound, slowly, in a way that is
maddening to diagnose.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the rest.

---

## S. Tarr

BENCO's audio/visual person, and the reason he turns up here rather than
R. Triy. He is drawn from the roster geometry rather than loaded from a file,
which is why he is never missing, is never blurry, and is the same shape in the
window, in the taskbar and in the `.ico`.

---

## Licence

MIT — see [LICENSE](LICENSE).

BENCsnip links FFmpeg (LGPL v2.1+, and GPL when built with `--enable-gpl`
components such as libx264), raylib (zlib/libpng), and embeds Terminus (TTF)
under the SIL Open Font License. [NOTICE](NOTICE) has the full attribution and
what it obliges you to do when you redistribute a binary; the information
window in the program shows the same text, which is how a binary-only copy
satisfies the OFL.
