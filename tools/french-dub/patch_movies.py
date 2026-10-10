"""Step 2: French movies.

For each Movies/*_english_pal.wmv of the Xbox 360 image, takes the French Xbox movie
MOVIES\\<NAME>_FRENCH_PAL.VP6 (packed in the ZZDATA files of the Xbox disc) and replaces the audio
track inside the original file (see asf.py):
- the 720p WMV3 video and the whole layout of the 360 file stay the same;
- the French audio (EA-XA R3, 5.1 or stereo, 48 kHz) is mixed down to stereo and encoded as WMA2,
  an audio format the game already plays in its own movies (ealogo).

The stereo mix L = FL + 0.7071*C + 0.7071*BL (same on the right, no LFE) gives, after the downmix of
the Switch port (0.586 front, 0.414 centre/rear), the original proportions.

Checks:
- before: music and sound effects must land at the same time as in the 360 version. The correlation
  peak of the front-left channel must be within 20 ms. The score is ~1.0 when only the centre
  (dialogue) changes; it is lower (0.3-0.8) in the tutorials, where the narrator is also in the front
  channels. If the French audio is longer than the video, the cut end must be silent;
- after: the written file is read back by FFmpeg and correlated with the English one again.

32 of the 36 movies have a French version on the Xbox disc; intro_movie, storyfmv_pal11,
storyfmv_rapal and eahd_bumper stay as they are.

Usage: python patch_movies.py <Xbox 360 image> <Xbox image> <output Movies folder> <ffmpeg> [names...]
(with names, e.g. ealogo storyfmv_raz08: only those movies, no report)
"""
import os
import re
import statistics
import struct
import subprocess
import sys
import tempfile

from alignment import offset
from asf import Asf
from ea_movie_audio import duration, split_stereo
from zzdata import open_discs, path

SUFFIX = "_english_pal.wmv"
# WMA2 bit rates tried, best first: the frames covering each original audio object must fit in its
# size (Microsoft's WMA Pro sometimes puts more than one second in one object)
BITRATES_KBPS = (192, 160, 128, 112, 96, 80, 64, 56, 48, 40, 32)
FRAME = 2048  # samples per WMA2 frame at 32 kHz and above
# Decoded Microsoft files are one frame late on their source; FFmpeg's WMA2 encoder is not
# (measured: +38 ms at 48 kHz, +42 ms at 44.1 kHz). So the audio is delayed by one frame.
DELAY = f"adelay={FRAME}S:all=1"
MIN_SCORE = 0.25
SILENCE_DB = -45
MIX = {
    # one pair: already stereo
    1: "[0:a]anull",
    # three pairs: 5.1 in EA order (FL, C, FR, BL, BR, LFE)
    3: ("[0:a][1:a][2:a]join=inputs=3:channel_layout=5.1:"
        "map=0.0-FL|0.1-FC|1.0-FR|1.1-BL|2.0-BR|2.1-LFE,"
        "pan=stereo|FL=FL+0.7071*FC+0.7071*BL|FR=FR+0.7071*FC+0.7071*BR"),
}


def ffprobe_duration(ffmpeg, file):
    folder, name = os.path.split(ffmpeg)
    ffprobe = os.path.join(folder, name.replace("ffmpeg", "ffprobe"))
    out = subprocess.run([ffprobe, "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", file],
                         check=True, capture_output=True, text=True).stdout
    return float(out)


def level_db(ffmpeg, inputs, filtergraph):
    """Overall RMS level (dB) of the French audio through `filtergraph`."""
    err = subprocess.run([ffmpeg, "-hide_banner", *inputs, "-filter_complex",
                          filtergraph + ",astats=measure_perchannel=none:measure_overall=RMS_level",
                          "-f", "null", "-"], check=True, capture_output=True, text=True).stderr
    values = [float(v) for v in re.findall(r"RMS level dB: (\S+)", err) if v != "-inf"]
    return max(values, default=float("-inf"))


def wma2_blocks(ffmpeg, inputs, filtergraph, seconds, rate, bitrate, tmp):
    """Encode stereo WMA2; returns (WAVEFORMATEX + codec data, block size, blocks)."""
    out = os.path.join(tmp, "fr.wma")
    filtergraph += (f",aresample={rate},{DELAY},apad,atrim=0:{seconds:.6f},"
                    "alimiter=limit=0.98:level=disabled[a]")
    subprocess.run([ffmpeg, "-v", "error", "-y", *inputs, "-filter_complex", filtergraph, "-map", "[a]",
                    "-c:a", "wmav2", "-b:a", f"{bitrate}k", "-f", "asf", out], check=True)
    with open(out, "rb") as f:
        enc = Asf(f.read())
    o, _ = enc.audio_stream()
    tsd = struct.unpack_from("<I", o, 64)[0]
    type_data = bytes(o[78:78 + tsd])
    block = struct.unpack_from("<H", type_data, 12)[0]
    stream = b"".join(pl.data for _, _, pieces in enc.audio_objects() for pl in pieces)
    return type_data, block, [stream[i:i + block] for i in range(0, len(stream) - block + 1, block)]


def schedule(sizes, bounds, block):
    """Spread the WMA2 frames over the original audio objects.

    bounds[k]: first frame that falls in object k according to its timestamp. An object cannot get
    more frames than its original size holds, nor fewer than one: the excess moves on to the next
    objects. Returns the frame ranges per object and the largest delay (in frames) on the timestamps.
    """
    ranges, given, delay = [], 0, 0
    for k, size in enumerate(sizes):
        n = max(1, min(size // block, bounds[k + 1] - given))
        ranges.append((given, given + n))
        given += n
        delay = max(delay, bounds[k + 1] - given)
    return ranges, delay


def convert(x360, xbox, file_path, vp6, output, ffmpeg):
    """Write the French movie to `output` and return the report line."""
    name = os.path.basename(file_path).lower()
    movie = xbox.read(vp6)
    pairs = split_stereo(movie)
    if len(pairs) not in MIX:
        return f"{name:45} SKIPPED: {2 * len(pairs)} channels not supported"
    original = x360.read_file(file_path)
    with tempfile.TemporaryDirectory(dir=output) as tmp:
        en = os.path.join(tmp, "en.wmv")
        with open(en, "wb") as w:
            w.write(original)
        inputs = []
        for i, stream in enumerate(pairs):
            p = os.path.join(tmp, f"fr{i}.asf")
            with open(p, "wb") as w:
                w.write(stream)
            inputs += ["-i", p]

        lag, score = offset(ffmpeg, ["-i", en], "[0:a:0]pan=mono|c0=c0", inputs[:2], "[0:a]pan=mono|c0=c0")
        length = ffprobe_duration(ffmpeg, en)
        gap = duration(movie) - length
        measure = f"offset {lag:+.3f} s, score {score:.2f}, length gap {gap:+.2f} s"
        if abs(lag) > 0.02 or score < MIN_SCORE:
            return f"{name:45} SKIPPED ({measure})"
        if gap > 0.05:
            end = level_db(ffmpeg, inputs, MIX[len(pairs)] + f",atrim=start={length}")
            if end > SILENCE_DB:
                return f"{name:45} SKIPPED ({measure}, cut end audible at {end:.0f} dB)"

        asf = Asf(original)
        objects = asf.audio_objects()
        o, _ = asf.audio_stream()
        rate = struct.unpack_from("<I", o, 78 + 4)[0]
        # WMA2 frames covering each original audio object, from its timestamps (ms)
        times = [pt for _, pt, _ in objects]
        step = statistics.median(b - a for a, b in zip(times, times[1:])) if len(times) > 1 else 100
        times.append(times[-1] + step)
        bounds = [round((t - times[0]) * rate / 1000 / FRAME) for t in times]
        sizes = [size for size, _, _ in objects]
        for bitrate in BITRATES_KBPS:
            ranges, delay = schedule(sizes, bounds, bitrate * 1000 * FRAME // (rate * 8))
            if delay <= 1:
                break
        else:
            return f"{name:45} ERROR: audio objects too small even at {BITRATES_KBPS[-1]} kb/s"
        type_data, block, blocks = wma2_blocks(ffmpeg, inputs, MIX[len(pairs)],
                                               (bounds[-1] + 1) * FRAME / rate, rate, bitrate, tmp)
        ranges, delay = schedule(sizes, bounds, block)
        if delay > 1 or len(blocks) < ranges[-1][1]:
            return f"{name:45} ERROR: {len(blocks)} WMA2 blocks of {block} bytes, delay {delay} frames"
        asf.set_audio_format(type_data, block)
        asf.replace_audio([b"".join(blocks[a:b]) for a, b in ranges])
        target = os.path.join(output, name)
        with open(target, "wb") as w:
            w.write(asf.to_bytes())

        # read back: the French audio must still land on the English one
        after, score2 = offset(ffmpeg, ["-i", en], "[0:a:0]pan=mono|c0=c0", ["-i", target], "[0:a:0]pan=mono|c0=c0")
    return (f"{name:45} OK ({measure}; WMA2 {bitrate} kb/s, max delay {delay * FRAME * 1000 // rate} ms, "
            f"read back: offset {after:+.3f} s, score {score2:.2f})")


def main():
    iso360, iso_xbox, output, ffmpeg = sys.argv[1:5]
    x360, xbox = open_discs(iso360, iso_xbox)
    os.makedirs(output, exist_ok=True)
    movies = sorted(p for p in x360.files if p.startswith("MOVIES/") and p.endswith(SUFFIX.upper()))
    only = sys.argv[5:]
    report = []
    for file_path in movies:
        name = file_path[len("MOVIES/"):-len(SUFFIX)]
        if only and name.lower() not in only:
            continue
        vp6 = path("MOVIES", f"{name}_FRENCH_PAL.VP6")
        if xbox.has(vp6):
            line = convert(x360, xbox, file_path, vp6, output, ffmpeg)
        else:
            line = f"{os.path.basename(file_path).lower():45} unchanged (no French version on the Xbox disc)"
        print(line, flush=True)
        report.append(line)
    if not only:
        with open(os.path.join(output, "movies_report.txt"), "w", encoding="utf-8") as w:
            w.writelines(line + "\n" for line in report)


if __name__ == "__main__":
    main()
