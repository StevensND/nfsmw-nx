"""Step 3: French in-game speech (police radio, Mia's text messages...).

All in-game speech is in SOUND\\SPEECH\\COPSPEECH_xx.{BIG,IDX,CSI,EVT}. The 360 ones (English,
MicroTalk 5:1 at 36 kHz, 13562 lines) are replaced by those of the French Xbox disc (MicroTalk 10:1
at 24 kHz, 9929 lines), re-encoded as EA-XA, a codec the 360 speech player accepts:
1. each Xbox line is decoded by vgmstream (vgmstream-cli);
2. it is resampled to 36000 Hz (FFmpeg, soxr), the rate of all the 360 lines, then encoded as EA-XA
   channel by channel (ea_xa.py; 96 lines are stereo) and wrapped as a 360 EA stream (ea_stream.py);
3. the .BIG is rebuilt bank by bank and the .IDX (speech_idx.py, converted to the 360 byte order) is
   updated: offset of each bank, offset of each line in its bank, unit of those offsets (alignment
   mask, unit = 256 * (mask + 1)) and bank size in that unit;
4. .CSI: the 360 one (same content as the French one apart from byte order);
   .EVT: the 360 one with the two language-specific counters (+12) of the French one.
The .BIG, .IDX and .EVT go into a new NFS/ZZDATA9.BIN (on the 360 disc ZZDATA9 only holds
COPSPEECH_EN.BIG) and NFS/ZDIR.BIN points to them. The ZDIR.BIN written by step 1 is reused if it is
in the output folder.

Decoding and encoding take a while (about 25 + 20 minutes); with --cache, the encoded lines are kept
in a local file. It contains game audio: do not share it.

Usage: python patch_speech.py <Xbox 360 image> <Xbox image> <output game_root> <ffmpeg> <vgmstream-cli>
       [--cache FILE]
"""
import io
import os
import pickle
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave
from concurrent.futures import ThreadPoolExecutor

import numpy as np

from ea_stream import header, stream
from ea_xa import FRAME, encode_batch
from speech_idx import FORMATS, read, to_360, unit
from zzdata import SECTOR, bstringhash, open_discs, path

SPEECH = path("SOUND", "SPEECH", "COPSPEECH_{}.{}")
ZZDATA = 9
BATCH = 512
BANK_ALIGNMENT = 2048
RATE = 36000


def _elements(pt):
    """Elements of a PT header: [(tag, value or None for markers)] up to 0xFF."""
    out, i = [], 4
    while i < len(pt):
        t = pt[i]
        if t == 0xFF:
            return out
        if t in (0xFC, 0xFD, 0xFE):
            out.append((t, None))
            i += 1
        else:
            n = pt[i + 1]
            out.append((t, bytes(pt[i + 2:i + 2 + n])))
            i += 2 + n
    raise ValueError("PT header without an end")


def list_streams(big):
    """[(offset, bytes of the SCHl..SCEl stream, header elements)] in file order."""
    out, p = [], 0
    while True:
        p = big.find(b"SCHl", p)
        if p < 0:
            return out
        q = p
        while True:
            tag, size = big[q:q + 4], struct.unpack_from("<I", big, q + 4)[0]
            q += size
            if tag == b"SCEl":
                break
        header_size = struct.unpack_from("<I", big, p + 4)[0]
        out.append((p, big[p:q], _elements(big[p + 8:p + header_size])))
        p = q


_counter = iter(range(1 << 30))
_lock = threading.Lock()


def decode(data, vgmstream, ffmpeg, folder):
    """Decode an EA stream with vgmstream and resample it to RATE; returns (int16 samples per channel, rate).

    vgmstream-cli rejects some names (underscores, ./ relative paths): simple names, in the
    working folder of the process.
    """
    with _lock:
        name = f"r{next(_counter)}.asf"
    file = os.path.join(folder, name)
    with open(file, "wb") as w:
        w.write(data)
    try:
        wav = subprocess.run([vgmstream, "-i", "-p", name], cwd=folder, check=True,
                             capture_output=True).stdout
    finally:
        os.remove(file)
    with wave.open(io.BytesIO(wav)) as w:
        if w.getsampwidth() != 2:
            raise ValueError("line is not 16-bit")
        channels = w.getnchannels()
        source_rate = w.getframerate()
        raw = w.readframes(w.getnframes())
    if source_rate != RATE:
        raw = subprocess.run([ffmpeg, "-v", "error", "-f", "wav", "-i", "pipe:0",
                              "-af", f"aresample={RATE}:resampler=soxr", "-f", "s16le", "pipe:1"],
                             input=wav, check=True, capture_output=True).stdout
    pcm = np.frombuffer(raw, dtype="<i2").reshape(-1, channels)
    return [pcm[:, c].copy() for c in range(channels)], RATE


def encode_all(signals):
    """Encode every signal as EA-XA, in batches of similar lengths."""
    order = sorted(range(len(signals)), key=lambda i: len(signals[i]))
    out = [None] * len(signals)
    for start in range(0, len(order), BATCH):
        batch = order[start:start + BATCH]
        n = max(len(signals[i]) for i in batch)
        n += -n % FRAME
        padded = [np.concatenate([signals[i], np.zeros(n - len(signals[i]), np.int16)]) for i in batch]
        data, _ = encode_batch(padded)
        for i, d in zip(batch, data):
            out[i] = d
        print(f"  encoded {min(start + BATCH, len(order))}/{len(order)}", flush=True)
    return out


def canonical_header(s):
    """Rewrite the SCHl header of a stream with ea_stream.header, the rest unchanged."""
    size = struct.unpack_from("<I", s, 4)[0]
    elements = _elements(s[8:size])
    values = {t: int.from_bytes(v, "big") for t, v in elements if v is not None}
    return header(elements, values[0x85], values[0x84]) + s[size:]


def _align(n, a):
    return (n + a - 1) // a * a


def rebuild(idx_be, offsets, streams):
    """Rebuild the .BIG and update the .IDX (big endian) with the new offsets.

    offsets: original offset of each line; streams: its new stream.
    """
    idx = bytearray(idx_be)
    banks = read(idx_be, ">")
    starts = sorted(bk["big"] for bk in banks)
    rank = {p: i for i, p in enumerate(offsets)}
    new = {}
    sizes = {}
    big = bytearray()
    for k, start in enumerate(starts):
        end = starts[k + 1] if k + 1 < len(starts) else float("inf")
        members = [p for p in offsets if start <= p < end]
        size = sum(_align(len(streams[rank[p]]), 256) for p in members)
        shift = 0
        while size >> (8 + shift) > 0x7FFF:  # signed values: EA's tool stops at 32767
            shift += 1
        bank_unit = 256 << shift
        base = _align(len(big), BANK_ALIGNMENT)
        big += bytes(base - len(big))
        for p in members:
            new[p] = (len(big), base, shift)
            big += streams[rank[p]]
            stream_end = len(big)
            big += bytes(_align(len(big) - base, bank_unit) - (len(big) - base))
        sizes[base] = -(-(stream_end - base) // bank_unit)  # bank size without the final padding
    for i in range(struct.unpack_from(">I", idx, 4)[0]):
        entry = 0x58 + 16 * i
        _, _, sub, old = struct.unpack_from(">4I", idx, entry)
        width = FORMATS[idx[sub + 4]]
        old_unit = unit(idx_be, sub)
        _, base, shift = new[old]
        struct.pack_into(">I", idx, entry + 12, base)
        idx[sub + 7] = (1 << shift) - 1  # alignment mask: unit = 256 * (mask + 1)
        if sizes[base] > 0x7FFF or struct.unpack_from(">H", idx, sub + 10)[0]:
            raise ValueError("bank size does not fit its 16-bit field")
        struct.pack_into(">H", idx, sub + 8, sizes[base])
        for e in range(idx[sub + 5]):
            field = sub + 12 + width * e
            v = struct.unpack_from(">H", idx, field)[0]
            p, b, _ = new[old + v * old_unit]
            struct.pack_into(">H", idx, field, (p - b) >> (8 + shift))
    return bytes(big), bytes(idx)


def encoded_lines(lines, vgmstream, ffmpeg, cache):
    if cache and os.path.exists(cache):
        with open(cache, "rb") as f:
            print(f"encoded lines read from {cache}")
            return pickle.load(f)
    t = time.time()
    with tempfile.TemporaryDirectory() as folder, ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        decoded = list(pool.map(lambda s: decode(s[1], vgmstream, ffmpeg, folder), lines))
    print(f"vgmstream decoding: {time.time() - t:.0f} s; stereo lines: "
          f"{sum(len(c) == 2 for c, _ in decoded)}")
    t = time.time()
    signals = [s for channels, _ in decoded for s in channels]  # each channel encoded on its own
    data = iter(encode_all(signals))
    streams = [stream(el, [next(data) for _ in channels], len(channels[0]), rate)
               for (_, _, el), (channels, rate) in zip(lines, decoded)]
    print(f"EA-XA encoding: {time.time() - t:.0f} s")
    if cache:
        with open(cache, "wb") as f:
            pickle.dump(streams, f)
    return streams


def main():
    args = sys.argv[1:]
    cache = None
    if "--cache" in args:
        i = args.index("--cache")
        cache = args[i + 1]
        del args[i:i + 2]
    iso360, iso_xbox, output, ffmpeg, vgmstream = args
    x360, xbox = open_discs(iso360, iso_xbox)
    nfs = os.path.join(output, "NFS")
    os.makedirs(nfs, exist_ok=True)

    idx_fr = xbox.read(SPEECH.format("FR", "IDX"))
    idx = to_360(idx_fr)
    if read(idx, ">") != read(idx_fr, "<"):
        raise SystemExit("converting the IDX changed its content")
    lines = list_streams(xbox.read(SPEECH.format("FR", "BIG")))
    print(f"COPSPEECH_FR.BIG: {len(lines)} lines")

    streams = [canonical_header(s) for s in encoded_lines(lines, vgmstream, ffmpeg, cache)]
    big, idx = rebuild(idx, [p for p, _, _ in lines], streams)

    def line_at(bk, v):
        p = bk["big"] + v * unit(idx, bk["pos"])
        return big[p:p + 4]
    if any(line_at(bk, v) != b"SCHl" for bk in read(idx, ">") for v in bk["offsets"]):
        raise SystemExit("an IDX offset does not land on a line")
    print(f"COPSPEECH .BIG rebuilt: {len(big)} bytes; IDX checked")

    evt = bytearray(x360.read(SPEECH.format("EN", "EVT")))
    evt[12:16] = struct.pack(">HH", *struct.unpack_from("<HH", xbox.read(SPEECH.format("FR", "EVT")), 12))

    placed = {}
    with open(os.path.join(nfs, f"ZZDATA{ZZDATA}.BIN"), "wb") as w:
        for ext, data in (("BIG", big), ("IDX", idx), ("EVT", bytes(evt))):
            placed[bstringhash(SPEECH.format("EN", ext))] = (w.tell() // SECTOR, len(data))
            w.write(data + bytes(_align(len(data), SECTOR) - len(data)))
        end = w.tell()

    zdir_file = os.path.join(nfs, "ZDIR.BIN")
    if os.path.exists(zdir_file):
        with open(zdir_file, "rb") as f:
            zdir = bytearray(f.read())
    else:
        zdir = bytearray(x360.zdir)
    base = x360.global_sector_base(ZZDATA)
    for i in range(0, len(zdir) - 23, 24):
        h, number, sector, _, size, checksum = struct.unpack_from("<6I", zdir, i)
        if h in placed:
            sector, size = placed.pop(h)
        elif number == ZZDATA:  # other ZZDATA9 entry (empty on the disc): at the end of the file
            sector = end // SECTOR
        else:
            continue
        struct.pack_into("<6I", zdir, i, h, ZZDATA, sector, base + sector, size, checksum)
    if placed:
        raise SystemExit("COPSPEECH_EN entries missing from ZDIR.BIN")
    with open(zdir_file, "wb") as w:
        w.write(zdir)
    print(f"ZZDATA{ZZDATA}.BIN: {end} bytes; ZDIR.BIN: COPSPEECH_EN.BIG, .IDX and .EVT -> French")


if __name__ == "__main__":
    main()
