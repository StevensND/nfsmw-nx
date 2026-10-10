"""EA audio (SCHl/SCDl) of the Xbox movies, made readable by FFmpeg.

The Xbox movies (.VP6, EA "MVhd" container) interleave the frames with an EA-XA R3 audio stream in
5.1 (48 kHz) or stereo. FFmpeg's EA demuxer refuses more than 2 channels ("Unsupported number of
channels: 6") while its adpcm_ea_r3 decoder can read them. So the stream is rewritten as separate
stereo EA audio files (channels 0-1, 2-3 and 4-5) that FFmpeg decodes and joins again.

SCDl block in EA-XA R3: sample count (u32 BE), one offset per channel (u32 BE, counted from the end
of the table), then the data of each channel.
"""
import struct


def chunks(data):
    pos = 0
    while pos + 8 <= len(data):
        tag = data[pos:pos + 4]
        size = struct.unpack_from("<I", data, pos + 4)[0]
        if size < 8 or pos + size > len(data):
            raise ValueError(f"bad chunk at {pos}: {tag!r} size {size}")
        yield tag, data[pos:pos + size]
        pos += size


def _set_channels(header, channels):
    """Replace element 0x82 (channel count) in the SCHl header."""
    i = header.find(b"\x82\x01")
    if i < 0:
        raise ValueError("no element 0x82 (channels) in the SCHl header")
    out = bytearray(header)
    out[i + 2] = channels
    return bytes(out)


def _channel_count(header):
    i = header.find(b"\x82\x01")
    return header[i + 2] if i >= 0 else 2


def _element(header, tag):
    """Value of a PT header element (tag, 1-byte length, big-endian value)."""
    i = header.find(bytes([tag]))
    while i >= 0:
        n = header[i + 1]
        if 1 <= n <= 4:
            return int.from_bytes(header[i + 2:i + 2 + n], "big")
        i = header.find(bytes([tag]), i + 1)
    return None


def duration(movie):
    """Length of the audio in seconds, from the SCHl header (0x85 samples, 0x84 sample rate)."""
    header = next(c for t, c in chunks(movie) if t == b"SCHl")
    samples = _element(header, 0x85)
    rate = _element(header, 0x84) or 48000
    return samples / rate


def split_stereo(movie):
    """List of stereo EA audio streams (bytes), one per channel pair."""
    parts = list(chunks(movie))
    header = next(c for t, c in parts if t == b"SCHl")
    channels = _channel_count(header)
    if channels % 2:
        raise ValueError(f"{channels} channels: odd counts are not supported")
    pairs = [bytearray() for _ in range(channels // 2)]
    for out in pairs:
        out += _set_channels(header, 2)
    for tag, chunk in parts:
        if tag == b"SCDl":
            payload = chunk[8:]
            samples = payload[:4]
            table = 4 + 4 * channels
            offs = [struct.unpack_from(">I", payload, 4 + 4 * c)[0] + table for c in range(channels)]
            ends = offs[1:] + [len(payload)]
            for p, out in enumerate(pairs):
                a = payload[offs[2 * p]:ends[2 * p]]
                b = payload[offs[2 * p + 1]:ends[2 * p + 1]]
                body = samples + struct.pack(">II", 0, len(a)) + a + b
                out += b"SCDl" + struct.pack("<I", 8 + len(body)) + body
        elif tag in (b"SCCl", b"SCEl"):
            for out in pairs:
                out += chunk
    return [bytes(p) for p in pairs]
