# BENCsnip architecture

## The split that matters

```
src/core/     sn_media.*      libav: probe, decode, seek, scale, resample
              sn_timeline.*   tracks, clips, and every edit anything can make
              sn_render.*     the timeline as pictures and sound
              sn_text.*       glyphs into pixels, for text on the picture
              sn_project.*    the .bencsnip file
              sn_export.*     the fast path and the rendering path
              sn_version.h    one place for the version number

src/gui/      sn_gui.*        theme and widget set
              sn_player.*     the decode thread and the audio clock
              sn_bin.*        the media pane and its thumbnail worker
              sn_track.*      the timeline pane
              sn_dialog.*     export, information, confirm, file browser
              sn_filedlg.*    the platform's own open/save dialogs
              sn_appmenu.*    the menu bar, and the list of what is in it
              *_mac.mm        the macOS halves of those two, in Objective-C++
              sn_tarr.*       the mascot
              main.cpp        layout, keyboard, and the glue
```

`src/core` does not include raylib, does not open a window and does not know
what a mouse is. It does now rasterise text, with a vendored stb_truetype -
because the exporter has to draw the same captions the preview does, and the
moment those are two pieces of code they are two answers. `make test` builds
and runs it on a machine with no screen and no sound card, which is what keeps
that rule honest. Everything a headless render would need is already there —
including the font, which is why the embedded assets live in `src/core`.

## Time

Seconds, in a double, everywhere above the media layer. At an hour a double
still resolves about a nanosecond, so the only thing needing care is comparing
two times that ought to be equal after arithmetic — hence one epsilon in
`sn_timeline.cpp`, used by everything in it.

Frames appear in exactly two places: the export loop, which walks
`from + n/fps`, and the timecode readout. There is no frame-numbered timeline,
because a timeline holding a 25 fps clip and a 59.94 fps clip has no single
frame number to be at.

## The data model

A project is a bin of files and a list of tracks of clips.

```
Clip  = { source, in, out, pos, gain, muted, link, repeat }
Track = { kind, clips, fx, gain, muted, locked, + a layout for the picture }
FxPoint = { t, v, hold }
```

A clip is a range of one file placed at a time. There is no transition object,
no nested sequence, no effect graph. Trimming moves `in` or `out`; sliding
moves `pos`; splitting makes two clips out of one. A cross-fade is two clips on
two video tracks with fades that overlap, which is what a cross-fade is anyway.

`link` is how a video clip and its audio stay together: clips sharing a nonzero
link id move, trim and delete as one, and selecting one selects both. A split
gives the two halves new link ids of their own, because after a cut they are
separate clips and dragging one should not drag the other.

A video track also carries where its picture goes: `scale`, `x`, `y` and four
crop fractions, all relative to the canvas rather than in pixels, so changing
the project's size moves nothing. It is per track rather than per clip because
what it is for is a layout — a small video in the corner of a big one, two
side by side — and a layout that changed halfway through a track would be a
different feature with a different interface.

An audio track carries a level of its own, which multiplies the level on each
of its clips: a clip at half on a track at half is a quarter. Same range and
same meaning as the clip's, so the two numbers read alike, and per track for
the reason a mixer has faders — pulling a whole track down should not mean
undoing what was set clip by clip.

Text is a clip too. A caption is a `Clip` on a `TRACK_TEXT` track with `source`
of 0 and a `TextStyle` on it — the words, the face, the size, where it sits,
how far it is turned, two colours — so it trims, slides, splits, fades, links
and mutes because clips already do all of that. Video and text are one *band*:
the list has always meant two things at once, and a caption has to be able to
sit in front of one video track and behind another, so what used to be "same
kind" for ordering is now `sameBand`, and video and text swap freely inside it.

The order of the video tracks is the compositing order, and **the top row is
the back**. That is the opposite of the convention every other editor uses; it
is what this one was asked for, and the only defence against the surprise is
that the reorder buttons say which direction is which.

**Effects are a curve on the track, not a property of the clip.** A track's
`fx` is a list of points — a time, a level, and whether it holds — with
straight lines between them. It applies to whatever the track produces: how
opaque the picture is on a video *or text* track, the level on an audio one.
One idea, three meanings, one piece of code, which is why a caption fades
exactly the way a picture does.

It got there in two steps, and both are worth knowing. Fades were first two
numbers on every `Clip`, which could say "from the start" and "to the end" and
nothing else — not across a cut, not in the middle, not twice. They then became
a list of ramps on the track, which could at least be put anywhere; but a ramp
with two ends is still the wrong unit, because the next thing anybody wants is
another point. Come in, stay, go out is three ramps or four points, and as
points any one of them drags without disturbing the others.

Two points are a fade. Four are a fade in and out. Sixty-five are a sine, which
is what the presets are for. Outside the curve the level *holds* at the nearest
point's value rather than springing back, because a fade out that undid itself
the moment it finished would be useless for what fades are mostly for.

Each track's clips are kept sorted by `pos` and never overlap. Dropping a clip
on top of another cuts a hole in the one underneath rather than layering — that
is what dragging one clip onto another visibly means, and it is what
`clear_range` in `sn_timeline.cpp` does.

Editing operations are free functions rather than methods, so that the three
things they all do — change the vector, keep it sorted, mark the project dirty
— live in one place per operation and cannot be half-done by a new method that
forgot one.

## Decoding

A `Source` is also per *channel*. A clip can play one channel of its file
rather than the mix, and a Source asked for one is configured for it — the
resampler is told to do no mixing and the channel is picked out afterwards,
because letting it downmix to stereo first makes anything past the second
channel unrecoverable. Two clips playing two channels of one file are two
Sources, which is the same argument as below.

A `Source` opens the same file twice: one `AVFormatContext` for its video
stream, another for its audio. One context has one read position, so a seek
made to find a video frame also moves the audio, and playing a clip whose audio
runs three seconds ahead of its video in the interleave turns into a seek
storm. Two contexts cost a file handle and some buffers; in exchange the halves
are simply independent.

`frameAt(t)` seeks to the keyframe at or before `t` and decodes forward —
except when `t` is less than two seconds ahead of where the decoder already
sits, where it just keeps decoding. Two seconds is longer than most GOPs and
much shorter than a seek plus re-decode, so scrubbing forward through a long
GOP does not start over from the keyframe on every frame.

`audioAt(t, n, dst)` is the mixer's shape: exactly `n` samples per channel,
starting at `t`, silence past the end. It keeps a fifo of whatever the last
decoded frame had left over, because a decoder hands out 1024-sample frames and
a caller that discards the remainder of each one produces a buzz at the block
rate rather than the sound of the file.

Everything comes out as RGBA8 at a requested size and interleaved stereo float
at 48 kHz. Rotation from the container's display matrix is applied on the way
out, so a portrait phone video is portrait everywhere above this line.

## Rendering

`sn::Renderer` answers one question — what does the timeline look and sound
like at time `t` — and both the preview and the exporter ask it. That is the
whole reason the file exists: the usual way an export comes out different from
the preview is two pieces of code that both know how fades work.

Video composites first track to last over opaque black — first is the top row,
which is the back. Each layer is cropped, then fitted into its scaled box,
then placed by `x` and `y` in whatever space is left over, then blended by its
fade. The crop is taken first because it changes the shape that gets fitted: a
16:9 source cropped to its middle third is a 16:3 layer, and fitting the
uncropped aspect and cutting afterwards would leave it the wrong size and off
centre. The decoder is asked for whatever size makes the *kept* part come out
at the fitted size, so swscale does the scaling and the crop is a
sub-rectangle rather than a second resample.

The preview asks for the canvas at the canvas's own aspect, not at the shape
of the pane it is drawn in. That did not matter while every layer filled the
frame — one picture fitted into any box looks the same — and it matters
completely now: the right half of a 16:9 project is not the right half of a
3:1 pane, and a side-by-side layout would preview as something the export
would never produce. Audio sums every unmuted audio clip
under the block, with fades evaluated per sample — a fade shorter than a block
would otherwise be a step, and a step in a gain is a click — then clips to
±1.0, so an overloaded mix sounds overloaded rather than broken.

Text is composited in the same list order as the pictures, from a cache of
rasterised captions kept per text track — a track's clips do not overlap, so
only one caption on it can be under the playhead. Building one lays out the
glyphs and grows the outline out of them by dilation, which is expensive and
runs when the caption changes rather than when the frame does.

A Renderer owns its open files and is not thread-safe. The player thread has
one and the exporter has its own; a decoder is a read position, and two threads
sharing one is two threads seeking against each other.

## Playback

Three threads.

**The worker** renders ahead: mixed audio into a two-second ring buffer,
composited pictures into a queue about four deep. It owns its own copy of the
project, so the GUI can go on editing while a decode is in flight, and it
decodes with no lock held — holding the mutex across a decode would stall the
GUI for as long as a seek into a long GOP takes, which is exactly when the GUI
most needs to keep drawing.

**The audio device** drains the ring on its own thread. An underrun writes
silence and does *not* advance the read cursor.

**The GUI** asks for the picture that matches the clock.

The clock is the read cursor: `position() = base + (read - readBase) / 48000`.
Sound that was never played does not move it, which is why an underrun drops a
frame instead of jumping the playhead forward. On a machine where the audio
device will not open at all, the worker advances the cursor itself from the
wall clock and nothing downstream changes.

An edit sends a new project to the worker, which throws away the queued
pictures but keeps the audio already in the ring. Dragging a clip sends a new
project on every frame of the drag; emptying the ring each time would mean
silence for as long as the mouse is moving, where keeping it means at most half
a second of sound from an arrangement that has since changed.

## Export

Two paths, and which one ran is visible in the UI.

**Stream copy** applies when the timeline is one clip of one file with nothing
done to it but a trim, and the output settings match the source. Packets are
copied across with their timestamps shifted; a four-gigabyte camera file is
trimmed in about a second and loses nothing. The cost is that a copy can only
begin at a keyframe, so the result may include up to a GOP before the in point.
The dialog says so before the user commits, and any setting that would change
the picture — size, frame rate, format — disqualifies it.

**Rendering** is everything else: an output frame at `from + n/fps`, composited
and encoded, interleaved against 1024-sample audio blocks by whichever stream
is furthest behind. A stream that has reached the end is treated as infinitely
far ahead, which is what stops a video that ends before its audio from writing
frames past the end of the export.

Audio goes through an `AVAudioFifo` because the mixer works in blocks of 1024
and an encoder wants its own frame size, and those are the same number only by
luck. Frame timestamps count samples handed to the encoder rather than blocks
taken from the mixer, so an encoder running at something other than 48 kHz does
not drift.

If a stream copy fails at the muxer — a container that will not hold those
codecs as they are — it falls back to rendering rather than reporting an error
the user would have to understand to act on.

## The interface

Immediate mode, drawn with raylib, no toolkit. The BENCO look is flat fills,
thin dim borders and small radii, which is what an immediate-mode renderer
produces by default and what a native widget set would have to be argued out of
at every step. A clip on a timeline is not in any toolkit anyway.

Two rules the timeline pane is built on:

**Nothing moves until the pointer does.** A drag is armed on mouse-down and
only acts once the pointer has travelled a few pixels, so a click that selects
a clip does not also nudge it half a frame.

**The playhead is where the sound is.** Scrubbing seeks the player rather than
moving a number the player later catches up with.

Icons are drawn from primitives rather than loaded, and every triangle goes
through one helper that fixes its winding: raylib culls one direction, and
getting it wrong draws nothing at all, silently.

Thumbnails are made on a worker thread. Decoding one means opening a file and
seeking — tens of milliseconds for an mp4 and much longer for a camera file on
a network share — and doing that on the frame a file is dropped means the
window stops responding at exactly the moment someone is dropping ten more.

## Assets

The font, the wordmark, the licences and the icon are compiled into the binary
by `tools/mkembed.c`. A file beside an executable is a file that can go
missing, and when the font went missing in the program this borrowed the idea
from, the window came up in a fallback face without saying so.

The licences are in there because embedding the font without them would not be
allowed: the OFL permits bundling provided every copy carries the notice in a
form the user can easily view. The information window is that form.

S. Tarr is drawn from the roster geometry wherever he appears *in* the
interface - the empty preview, the export dialog, the about window - so he is
never missing and never blurry.

The program's icon is not him alone: it is `film_camera_star.svg`, S. Tarr in
front of a film camera, and every size is generated from that one file by
`make icons`. Four of them are compiled into the binary for the window and the
taskbar; the same set becomes the `.ico` in the executable's resources and in
the installer, and the `.icns` in the macOS bundle. Below 32 pixels the film
strip is removed and the camera scaled up to fill the tile - simplifying
rather than shrinking, which is what an icon set does at that size.
