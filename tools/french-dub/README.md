# French dub for the PAL (English) edition

Tools that turn the **PAL (English)** Xbox 360 edition into a French version — text, movies and
in-game speech — using the player's own **original Xbox French disc** of *Need for Speed: Most
Wanted*. Tested on the Switch with this port.

No game files are included. The tools read two disc images that the player owns and write the files
to copy over `game_root`. **Those output files contain game data: do not share them.**

| Part | Result |
|---|---|
| Menus and game text | French (already on the 360 disc) |
| Movies | 32 of 36 dubbed in French, with the original 720p video |
| In-game speech (police radio, Mia's messages…) | French |

`intro_movie`, `storyfmv_pal11`, `storyfmv_rapal` and `eahd_bumper` have no French version on the
Xbox disc and stay in English.

## What you need

- Your Xbox 360 disc image of the **PAL (English)** edition (`default.xex` SHA-256
  `e2ea2ff1…10474`, as in the installer manifest), already installed with the installer page.
- Your **original Xbox** disc image of the **French** edition.
- Python 3 with numpy.
- FFmpeg with ffprobe next to it (WMA2 encoder, `soxr` resampler).
- [vgmstream](https://github.com/vgmstream/vgmstream/releases) `vgmstream-cli` (tested with r2117),
  to decode the Xbox MicroTalk speech.

## Usage

All in one go (asks for the two disc images and where to save, about an hour):

```sh
python build_package.py [--x360 ISO] [--xbox ISO] [--out nfsmw-nx-vf.zip] [--ffmpeg FFMPEG] [--vgmstream CLI]
```

It writes a zip holding `nfsmw-nx/game_root/...`, to extract into `sdmc:/switch/` like the installer
page's zip. The three steps can also be run on their own:

```sh
python patch_text.py   <360.iso> <xbox.iso> out
python patch_movies.py <360.iso> <xbox.iso> out/Movies <ffmpeg>
python patch_speech.py <360.iso> <xbox.iso> out <ffmpeg> <vgmstream-cli> [--cache speech.pkl]
```

Run `patch_text.py` before `patch_speech.py`: both change `NFS/ZDIR.BIN`, and the second one starts
from the first one's output. Then copy `out/` over `/switch/nfsmw-nx/game_root/` (back up the files
first):

| File | Size |
|---|---|
| `NFS/ZDIR.BIN` | 33 KB |
| `NFS/ZZDATA0.BIN` | 381 MB |
| `NFS/ZZDATA9.BIN` | 460 MB |
| `Movies/*.wmv` | 32 files, about 1.1 GB |

`patch_speech.py` takes about 25 minutes to decode and 20 to encode; `--cache` keeps the encoded
lines (game audio — do not share that file either).

## How it works

All three discs share the same packed file system: `NFS/ZDIR.BIN` is an index of 24-byte entries
(bStringHash of the upper-case path, ZZDATA number, sector, global sector — the sum of the sectors
of the previous ZZDATA files plus the sector —, size, checksum) and `NFS/ZZDATA*.BIN` hold the data.
The checksum is not a standard CRC32 and is not checked. 1349 file names are common to the Xbox and
360 discs.

### Text (`patch_text.py`)

The 360 PAL (English) disc already has `LANGUAGES\FRENCH.BIN`, but its executable loads
`ENGLISH.BIN`. `user_language` does not help: `XGetLanguage` in the SDK always returns English
(`sdk/src/kernel/xam/xam_info.cpp`, a TODO inherited from Xenia).

1. In `ZDIR.BIN`, the entry of `ENGLISH.BIN` points to the data of `FRENCH.BIN`. The same is done for
   the UTF-16 system messages (`LOCH` files 21d83a59 → a8a7b105, "Loading %1…").
2. That alone was not enough: `GLOBAL\GLOBALMEMORYFILE.BIN`, a pack preloaded at startup, holds
   copies of these files and the game uses them first. Their entries in the pack's table are
   renamed to a hash that does not exist, so the game falls back to `ZDIR.BIN`. Pack table: 8 zero
   bytes, magic `0x53219999`, count, then 20 bytes per entry (hash, offset, size in memory, stored
   size, flags).

### Movies (`patch_movies.py`, `asf.py`, `ea_movie_audio.py`, `alignment.py`)

The French Xbox movies (`MOVIES\<NAME>_FRENCH_PAL.VP6`) have the same mix as the 360 ones: music and
effects line up at 0 ms with a correlation score of 1.0, only the centre (dialogue) differs. Their
EA-XA R3 audio is 5.1 at 48 kHz; FFmpeg's EA demuxer refuses more than 2 channels, so it is split
into stereo pairs first.

- **First attempt, crashed:** remuxing with FFmpeg's ASF muxer. The game's WMV player got
  "fotograma comprimido incompleto" on frame 6 of `ealogo`, then an access violation. It needs the
  original layout (8000/17864-byte packets, 10 bytes of replicated data for video).
- **Working:** `asf.py` reads the original file and writes it back packet by packet (an untouched
  file comes back identical). Video payloads, packet count and index are unchanged; only the audio
  objects are refilled with WMA2 (an audio format the game already plays, in `ealogo`). The French
  audio is mixed to stereo with the same weights as the port's downmix, delayed by one frame
  (Microsoft's encoder has one frame of delay, FFmpeg's has none) and spread over the original
  audio objects following their timestamps. Some WMA Pro objects cover more than a second, so the
  bit rate is chosen per movie (192 kb/s for most). Every movie is checked by correlation before and
  after: 2-5 ms.

### In-game speech (`patch_speech.py`, `speech_idx.py`, `ea_xa.py`, `ea_stream.py`)

All in-game speech is in `SOUND\SPEECH\COPSPEECH_xx.{BIG,IDX,CSI,EVT}`: 9929 French lines
(MicroTalk 10:1, 24 kHz implied) against 13562 English ones (MicroTalk 5:1, 36 kHz). The `.CSI` and
`.EVT` files have the same content in both versions apart from byte order (the `.EVT` has two
language-specific counters at +12).

The `.IDX` format is documented in `speech_idx.py`. In short: a table of banks, then per bank an
alignment mask (unit = 256 × (mask + 1) bytes), the bank size in units, a list of line offsets
(big endian on both consoles) and, after it, as many u32 as the list format number, in the
console's byte order. Rebuilding the French and English indexes from their own lines gives back the
original bytes.

The French lines are decoded with vgmstream, resampled to 36 kHz and encoded as EA-XA, the codec of
the 360's own `nisaudio.big` streams (`ea_xa.py` is checked bit for bit against vgmstream's decoder).
The `.BIG` and `.IDX` are rebuilt and placed in a new `ZZDATA9.BIN`.

Attempts, in order (each one tested on the Switch):

| | Change | Result |
|---|---|---|
| A1 | Xbox `.BIG`/`.IDX` in 360 byte order, MicroTalk kept, 22050 Hz declared | silent; the log shows the right line being read |
| A2 | 24000 Hz (vgmstream's Xbox default), codec labelled MicroTalk 5:1 | silent |
| A3 | + French `.EVT` counters | silent |
| A4 | MicroTalk declared at 36000 Hz | silent |
| B | decoded, re-encoded as EA-XA 24 kHz, `.BIG`/`.IDX` rebuilt | silent (bank size field not updated) |
| R | original **English** speech placed the same way (new `ZZDATA9`) | English speech: the placement is fine |
| B2 | EA-XA resampled to 36 kHz, bank size fixed, 360-style headers | silent (mask written as a shift) |
| B3 | alignment mask fixed | silent; whole line read |
| X3 | **English** lines through the whole EA-XA pipeline | English speech: the pipeline is fine |
| B4 | u32 after each list converted to big endian | **French speech** |

A1–A4 had the same index problem as B–B3, so they do not show whether the 360 can play the Xbox
MicroTalk.

## Notes for a future version

- The game first tries to open each file outside the archives (`NtCreateFile` on
  `D:\Global\GlobalMemoryFile.bin`, `D:\Sound\Speech\copspeech_en.big`… in the log). Loose files in
  `game_root` could replace the 381 MB and 460 MB ZZDATA copies. Not tested yet.
- The installer page could do this in the browser (vgmstream has a WebAssembly build).
- `XGetLanguage` could return `user_language` instead of always English.
