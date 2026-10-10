"""EA SCHl (version 3) audio streams in EA-XA (mono or stereo), Xbox 360 layout.

  SCHl: PT header (platform 9 = Xbox 360, tag/length/big-endian value elements, 0xFF at the end).
        Without element 0xA0, the default Xbox 360 codec is EA-XA (vgmstream).
  SCCl: number of SCDl blocks (u32 big endian)
  SCDl: samples in the block (u32 BE), offset of each channel after this table (u32 BE), then the
        EA-XA frames of each channel
  SCEl: end of stream
As in the 360's own nisaudio.big streams: the first frame is raw (0xEE), the ADPCM state carries on
from one block to the next, blocks of 3192 samples (114 frames).
"""
import struct

BLOCK_SAMPLES = 3192
FRAME = 28


def _chunk(tag, body):
    return tag + struct.pack("<I", 8 + len(body)) + body


def _value(n):
    """Big-endian integer on 3 bytes (4 above), as in the 360 streams."""
    return n.to_bytes(3 if n < 1 << 24 else 4, "big")


def header(elements, samples, rate):
    """elements: original [(tag, value or None)]; version, codec, sample rate and sample count are
    dropped and ours are added the way the 360 streams do: sample count (0x85) then sample rate
    (0x84), 3 bytes each."""
    body = bytearray(b"PT\x09\x00")
    for t, v in elements:
        if t in (0x80, 0xA0, 0x84, 0x85):
            continue
        body += bytes([t]) if v is None else bytes([t, len(v)]) + v
        if t == 0xFD:
            for tag, n in ((0x85, samples), (0x84, rate)):
                body += bytes([tag, len(_value(n))]) + _value(n)
    body.append(0xFF)
    body += bytes(-len(body) % 4)
    return _chunk(b"SCHl", bytes(body))


def stream(elements, channels, samples, rate):
    """channels: for each channel, its EA-XA frames end to end (the first one raw, 61 bytes, then
    15 bytes per frame). The channel count (element 0x82) stays the original one."""
    per_block = BLOCK_SAMPLES // FRAME
    raw = 61
    blocks, pos, frame = [], 0, 0
    while frame * FRAME < samples:
        nb = min(per_block, -(-(samples - frame * FRAME) // FRAME))
        size = nb * 15 + (raw - 15 if frame == 0 else 0)
        n = min(BLOCK_SAMPLES, samples - frame * FRAME)
        pieces = [c[pos:pos + size] for c in channels]
        table = struct.pack(">I", n) + b"".join(struct.pack(">I", size * i) for i in range(len(channels)))
        body = table + b"".join(pieces)
        blocks.append(_chunk(b"SCDl", body + bytes(-len(body) % 4)))
        pos += size
        frame += nb
    return (header(elements, samples, rate)
            + _chunk(b"SCCl", struct.pack(">I", len(blocks)))
            + b"".join(blocks)
            + _chunk(b"SCEl", b""))
