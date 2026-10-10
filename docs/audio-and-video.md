# Audio and video on the Switch

This page explains how the game's sound and its cutscene videos were made to work on the Switch. Read it if a port
has gaps in the sound, a "robotic" sound, sound that stops, or slow videos. Audio problems showed up before frame
rate problems and took the longest to close.

Almost everything here is in the audio path of [ReXGlue](glossary.md#rexglue) or in the game's video player, so most
of it applies to other ports too. The audio path has three parts:

- **XAudio**, the Xbox 360's sound mixing library, which the game uses to play its sounds;
- **[XMA](glossary.md#xma) decoding**: XMA is the Xbox 360's compressed audio format, and the runtime decodes it with
  FFmpeg, an open source audio and video library;
- **the Switch output driver**, which sends the mixed sound to the console's audio output.

The settings named on this page go in `nfsmw.toml`, next to the NRO (see [nfsmw.toml](glossary.md#nfsmwtoml)).

## In short

- The output driver needed a steady "pump" that asks the game for sound at a fixed rate, and a small queue of mixed
  sound.
- The "robotic" sound was the game's audio server thread arriving late. It took three fixes: a higher priority for
  that thread, native versions of the hottest audio functions, and XMA decoding moved off the game's thread.
- An intermittent hang of the audio server was fixed with a timeout that also retries.
- Races have no music: still open (see below).
- Cutscenes are decoded with FFmpeg instead of the game's own WMV3 decoder, which was far too slow. This includes
  videos re-encoded by fan translations, which the game's decoder cannot play.
- FFmpeg no longer rebuilds the same math tables for every new sound.

## Output driver

The output driver is `switch_audio_system.cpp`, in the SDK (`sdk/src/audio/switch/`). It opens audout (the Switch's
audio output service) at 48 kHz, stereo, 16-bit, with three 1,024-sample buffers. Two things were needed on top of
what it did at first:

- **A pump at 187.5 Hz.** It asks the game for one frame of mixed sound at a steady rate, 187.5 times per second.
  Without it, the driver asked the game for four frames in a row and then nothing, which starved the game's voice (its
  stream of sound, see below) between bursts. The setting `audio_switch_bomba` turns the pump on; it is on by default.
- **A small queue of mixed frames**: 10 frames, 53 ms (setting `audio_switch_tramas_en_cola`). It absorbs short
  delays of the game's audio thread without adding a delay you can hear.

The game mixes six channels, and the driver folds them into two (the *downmix*): 0.586 for the front channels, 0.414
for the centre and rear, nothing from the LFE (subwoofer) channel, and hard clipping (values above the maximum are
cut off). Measured on HDMI captures, the mix peaks at 0.93-0.95 and never clips.

## "Robotic" audio

**Symptom:** a metallic, stuttering sound everywhere. It was worse in crashes and in busy areas, and it did not
depend on the frame rate.

**Cause:** the game has an *audio server thread* that prepares its sound in packets of 256 samples. It keeps a ring
of two packets (10.7 ms) for its XAudio *voice* (the stream of sound it plays through XAudio), and each mixed frame
uses one packet. When the server is late, the frame goes out with that voice silent. The result is 5.33 ms gaps
between bits of sound. No sound is lost: silence is inserted.

It took three separate fixes. Here they are from the one that helped most to the one that helped least.

### 1. A higher priority for the audio server

The server was a [guest](glossary.md#guest) thread at priority 0x3B. It took turns, in 10 ms slices, with the main
game thread and another busy guest thread, and the GPU ring thread ran above it (see
[platform-notes.md](platform-notes.md#threads) for how priorities work on the Switch). In crashes it delivered only
89 % of its packets.

- The server now runs at 0x2D (setting `nfsmw_audio_servidor_prioridad`).
- The audio worker (the thread that mixes each audio frame) may wait up to 30 ms for a late packet, instead of mixing
  that voice as silence (setting `nfsmw_audio_esperar_servidor_ms`).

Both are in `app/src/nfsmw_audio_servidor.cpp`. Together they made the problem "barely noticeable".

The SDK's priority table also had a bug. It compared thread names exactly, but thread names get a ` (F80000xx)`
suffix, so the audio worker and the XMA decoder never got the priority meant for them. It now compares with
`starts_with` (in `sdk/src/system/xthread.cpp`).

### 2. Native versions of the hottest audio functions

Two resamplers (they change the sample rate of a sound), a filter, a gain sum (it adds a sound into the mix at a
given volume) and `memset` were rewritten in C++ as [native replacements](glossary.md#native-replacement) of the
recompiled code. The C++ does the same floating-point operations in the same order, so the results are identical:
for example `double(float(std::fma(...)))`, and the PowerPC instruction `fnmsubs` written as `-std::fma(x, y, -z)`.
Over 159 million samples there were zero differences. The server's CPU time dropped by 17.5 %.

The code is in `app/src/`: `nfsmw_audio_remuestreo.cpp` (resamplers), `nfsmw_audio_filtro.cpp` (filter),
`nfsmw_audio_suma.cpp` (gain sum) and `nfsmw_crt_nativo.cpp` (`memset`).

### 3. XMA decoding off the game's thread

To start decoding a sound, the game calls `XMAEnableContext`. (An XMA *context* is one decoder slot with its sound;
the call writes the context's *kick* register, which tells the decoder to start.) The SDK decoded the context right
there, on the game's audio thread. On the Switch that cost 17.3 % of a core, with peaks of 14.4 ms against a 10.7 ms
buffer, and the GPU ring thread interrupted the decoding halfway.

Nothing in the game calls `WaitForWorkDone()`: the game polls the context (it checks it again and again). So the
decoding does not have to happen inside the kick. Now the kick only marks the context in a bitmap (one bit per
context) and wakes the XMA worker thread, which decodes it. The worker's full sweep over all contexts stays as a
safety net. The code is in `sdk/src/audio/xma_decoder.cpp`. Its setting, `audio_xma_en_trabajador`, chooses which
thread decodes; on the Switch it is the worker by default.

Results on the console:

- the game's audio thread went from 17.3 % to 1.5 % of a core;
- gaps in races went from 43 to 5;
- the data is ready 93 µs after the kick on average (the worst case, 8.3 ms, did not cause a gap);
- as a side effect, the race frame rate went up by 3.5 FPS.

### How it was measured

These tools made the problem measurable:

- counters of empty mixing passes and of delivered packets, every 500 ms;
- the time from the kick to the decoded data;
- a WAV dump of the game's six-channel sound before the driver;
- dumps of each XMA context;
- a gap detector, run over the audio track of videos captured from the console.

## An intermittent hang of the audio server

In about three of seven early runs, the game went completely silent and the race never finished loading.

**Cause:** the audio server thread (the recompiled function `sub_825E3E28` in this game) waits for its event with no
timeout. When it wakes up and its slot is still busy, it goes back to waiting without trying to deliver. So a single
lost end-of-packet notification stops it for good, and with it the game's queue of audio commands.

**Fix:** a timeout on that wait. After 250 ms without progress, the thread retries the delivery and frees the slot.
A timeout alone is not enough: the loop must also retry. The code is in `app/src/nfsmw_audio_servidor.cpp` (setting
`nfsmw_audio_rescate`, on by default).

**Lesson:** when a failure is intermittent, one run with working audio does not prove that a build is good.

## No music in races (open)

**Symptom:** some races have their engines, crashes and police radio, but no music. Sessions with music (runs 9
and 10) played a 2-channel track (335-352 MiB of `MW_MUSIC.MUS`); every session without it (runs 7 and 8, the career
loaded into free roam) played 4-channel streams (495-553 MiB, about 100 s each, two stereo layers). The problem is
specific to the 4-channel music.

**What is known:** the 4-channel music is read from the disc at the speed it plays, so the game's music player runs;
the sound is lost after that. The game decodes EA-XA itself, with a family of decoders. `sub_82618D60` decodes the
2-channel music (it shows in the sound thread's CPU profile when that music plays). `sub_82618788` and `sub_826186B8`,
called from a different group of functions (`sub_826132xx-826137xx`), are the candidates for the 4-channel streams.

**Ruled out:**

- The runtime's XMP notification (XMP is the console's music player service). Its parameter was inverted
  (`sdk/src/kernel/xam/apps/xmp_app.cpp`, fixed on 2026-10-10), but the game never sends the message that triggers it.
- The native resampler, filter and gain sum: validated against the recompiled code with 0 differences (run 8).
- The output downmix: the stereo fold keeps the surround channels at -3 dB, so music in the rear channels would
  still be heard.

**Probably by design (2026-10-10, parked):** the sessions without music were the career's prologue, up to the BMW
being taken; the user reports music starts once free roam is unlocked, which matches the original game's prologue
using its own score. Not confirmed against an Xbox 360.

**Diagnostic kept:** `nfsmw_audio_diag_musica` hooks the decoder `sub_82618788` and logs, every 10 s, the samples it
produced and their peak (`[musica]` lines). It has not run yet in a session with 4-channel music. The trail is in
[deko3d-devlog.md](deko3d-devlog.md).

## Videos

The game's cutscenes are [WMV3](glossary.md#wmv3) videos. The game's WMV3 decoder, recompiled from the Xbox 360
libraries, took 98 % of a core on the Switch and produced only about 22 frames per second of 720p30 video (1280x720
at 30 frames per second). So videos stuttered and their audio ended before the picture.

The port now gives the same bytes to FFmpeg's WMV3 decoder instead. It reads them through the game's own data
callbacks, and writes the decoded image planes (the Y, U and V parts of each frame) where the game expects them.
Videos now take 7-15 ms per frame. The code is in `app/src/nfsmw_video_nativo.cpp` and `app/src/nfsmw_video_wmv3.cpp`,
and the setting `nfsmw_video_wmv3_nativo` turns it on (on by default).

- **License.** FFmpeg is linked under the LGPL 2.1 (the SDK's build, configured without GPL parts). Do not link a GPL
  build.
- **Traps:**
  - identify the video by the stream being decoded, not by the last `.wmv` opened: the game opens the next one early;
  - read through the game's data callback instead of opening the file separately;
  - leave the "data remaining" count where the game expects it, or the game drops the next frame.
- **Checking.** A shadow mode (setting `nfsmw_video_wmv3_sombra`, off by default) decodes with both decoders and
  compares them. The videos were identical bit for bit, except for three short sections of one video with a mean
  difference of 0.02.
- **Re-encoded videos (fan translations).** The game's own videos only have I frames (complete pictures) and P frames
  (the changes from the previous picture), and they do not use the loop filter (a smoothing filter that WMV3 can
  apply to each picture). A Brazilian Portuguese fan translation re-encoded its dubbed videos with B frames (built
  from the pictures before and after them) and with the loop filter. The game's recompiled decoder cannot decode
  them: it leaves the picture empty, which shows as green, and it takes up to 330 ms per frame even on a PC. So the
  port decodes these videos with FFmpeg too:
  - the game decodes B frames with a third function (the one at `[ctx+3016]`), which the port replaces as well;
  - FFmpeg returns each frame from the same call that decodes it (`AV_CODEC_FLAG_LOW_DELAY`, and `has_b_frames`
    set to 0 after opening the decoder: without that, FFmpeg holds back the first frame);
  - if FFmpeg fails on one of these videos, the last good picture is shown again until the next I frame, because
    the game's decoder cannot be used as a fallback with them.

  The setting `nfsmw_video_wmv3_b_diag` (off by default) logs which picture planes and which fields of the decoder
  context each frame changes. It was used to find where the game leaves each type of frame.
- **Frame counters read 30.** During videos the game presents 30 frames per second, so frame counters read 30, not 60.

## Codec reinitialisation cost

Each XMA context opens FFmpeg's decoder again for every new sound, and every time FFmpeg rebuilt the same math
tables. In busy moments, `ff_mdct_init`, the split-radix permutation and the sine/cosine tables took about 28 % of
the XMA thread. Now FFmpeg's FFT and MDCT code keeps the bit-reversal table and the twiddle factors in a cache, so
they are built only once. That removed the cost. The change is in `sdk/thirdparty/FFmpeg/libavcodec/fft_template.c`
and `mdct_template.c`, in the parts marked `NFSMW`.
