"""Replace the audio track of a WMV file while keeping the original file layout.

The game's WMV player (recompiled from the Xbox 360 libraries) is strict about how the file is cut:
a WMV rebuilt by FFmpeg's ASF muxer (3200-byte packets, 8 bytes of replicated data) gives it
incomplete frames ("fotograma comprimido incompleto") and the game crashes. So the original file is
kept packet by packet:
- video payloads stay byte for byte the same, in the same packets, with the same replicated data;
  the header and the index (time -> packet number) stay valid;
- each audio object (one WMA block of the original) is filled with the new WMA2 blocks that cover
  the same time. They are smaller: the pieces of the object are shortened in proportion and the
  packet is completed with padding;
- only the audio stream properties (format, bit rate, block size) change in the header.
Reading then writing an untouched file gives back the same bytes.
"""
import struct
import uuid

HEADER = uuid.UUID("75b22630-668e-11cf-a6d9-00aa0062ce6c")
DATA = uuid.UUID("75b22636-668e-11cf-a6d9-00aa0062ce6c")
FILE_PROPERTIES = uuid.UUID("8cabdca1-a947-11cf-8ee4-00c00c205365")
STREAM_PROPERTIES = uuid.UUID("b7dc0791-a9b7-11cf-8ee6-00c00c205365")
AUDIO_MEDIA = uuid.UUID("f8699e40-5b4d-11cf-a8fd-00805f5c442b")
VAR_SIZE = (0, 1, 2, 4)


def _guid(b, p=0):
    return uuid.UUID(bytes_le=bytes(b[p:p + 16]))


def _read_var(b, p, t):
    n = VAR_SIZE[t]
    return (int.from_bytes(b[p:p + n], "little") if n else 0), p + n


def _write_var(v, t):
    n = VAR_SIZE[t]
    if n and v >= 1 << (8 * n):
        raise ValueError(f"{v} does not fit in {n} bytes")
    return v.to_bytes(n, "little") if n else b""


class Payload:
    __slots__ = ("stream", "key", "obj", "off", "rep", "data")

    def __init__(self, stream, key, obj, off, rep, data):
        self.stream, self.key, self.obj, self.off, self.rep, self.data = stream, key, obj, off, rep, data


class Packet:
    __slots__ = ("ec", "lt", "pp", "seq", "send", "dur", "plt", "payloads")


class Asf:
    """An ASF file in memory: header objects, data packets and what follows them."""

    def __init__(self, data):
        if _guid(data) != HEADER:
            raise ValueError("not an ASF file")
        _, count = struct.unpack_from("<QI", data, 16)
        self.header_reserved = data[28:30]
        self.objects = []
        p = 30
        for _ in range(count):
            size = struct.unpack_from("<Q", data, p + 16)[0]
            self.objects.append(bytearray(data[p:p + size]))
            p += size
        if _guid(data, p) != DATA:
            raise ValueError("no data object after the header")
        self.data_head = bytearray(data[p:p + 50])
        total = struct.unpack_from("<Q", data, p + 40)[0]
        p += 50
        self.packet_size = self._packet_size()
        self.packets = []
        for _ in range(total):
            self.packets.append(self._parse_packet(data[p:p + self.packet_size]))
            p += self.packet_size
        self.trailer = data[p:]

    def _packet_size(self):
        for o in self.objects:
            if _guid(o) == FILE_PROPERTIES:
                pmin, pmax = struct.unpack_from("<II", o, 92)
                if pmin != pmax:
                    raise ValueError("variable-size packets are not supported")
                return pmin
        raise ValueError("no file properties object")

    def _parse_packet(self, b):
        k = Packet()
        ec = b[0]
        if not ec & 0x80 or ec & 0x60:
            raise ValueError(f"unsupported error correction data ({ec:#x})")
        n = 1 + (ec & 0x0F)
        k.ec = bytes(b[:n])
        q = n
        k.lt, k.pp = b[q], b[q + 1]
        q += 2
        plen, q = _read_var(b, q, (k.lt >> 5) & 3)
        k.seq, q = _read_var(b, q, (k.lt >> 1) & 3)
        pad, q = _read_var(b, q, (k.lt >> 3) & 3)
        k.send, k.dur = struct.unpack_from("<IH", b, q)
        q += 6
        end = (plen or self.packet_size) - pad
        multi = k.lt & 1
        count, k.plt = 1, 0
        if multi:
            count, k.plt = b[q] & 0x3F, (b[q] >> 6) & 3
            q += 1
        k.payloads = []
        for _ in range(count):
            sn = b[q]
            q += 1
            obj, q = _read_var(b, q, (k.pp >> 4) & 3)
            off, q = _read_var(b, q, (k.pp >> 2) & 3)
            rlen, q = _read_var(b, q, k.pp & 3)
            if rlen == 1:
                raise ValueError("compressed payloads are not supported")
            rep = bytes(b[q:q + rlen])
            q += rlen
            if multi:
                ln, q = _read_var(b, q, k.plt)
            else:
                ln = end - q
            k.payloads.append(Payload(sn & 0x7F, bool(sn & 0x80), obj, off, rep, bytes(b[q:q + ln])))
            q += ln
        return k

    def _write_packet(self, k):
        body = bytearray()
        multi = k.lt & 1
        if multi:
            body.append(len(k.payloads) | (k.plt << 6))
        for pl in k.payloads:
            body.append(pl.stream | (0x80 if pl.key else 0))
            body += _write_var(pl.obj, (k.pp >> 4) & 3)
            body += _write_var(pl.off, (k.pp >> 2) & 3)
            body += _write_var(len(pl.rep), k.pp & 3)
            body += pl.rep
            if multi:
                body += _write_var(len(pl.data), k.plt)
            body += pl.data
        seq_t = (k.lt >> 1) & 3
        # the original padding length type first (same file if nothing changes)
        for pad_t in ((k.lt >> 3) & 3, 0, 1, 2, 3):
            head = len(k.ec) + 2 + VAR_SIZE[seq_t] + VAR_SIZE[pad_t] + 6
            pad = self.packet_size - head - len(body)
            if pad == 0 if pad_t == 0 else 0 <= pad < 1 << (8 * VAR_SIZE[pad_t]):
                break
        else:
            raise ValueError("packet too full")
        lt = (k.lt & 0x07) | (pad_t << 3)  # no explicit packet length
        out = bytearray(k.ec) + bytes([lt, k.pp])
        out += _write_var(k.seq, seq_t) + _write_var(pad, pad_t) + struct.pack("<IH", k.send, k.dur)
        out += body + bytes(pad)
        return bytes(out)

    def audio_stream(self):
        """(stream properties object, stream number) of the audio stream."""
        for o in self.objects:
            if _guid(o) == STREAM_PROPERTIES and _guid(o, 24) == AUDIO_MEDIA:
                return o, struct.unpack_from("<H", o, 72)[0] & 0x7F
        raise ValueError("no audio stream")

    def audio_objects(self):
        """Audio objects in file order: [(size, time in ms, [payloads])]."""
        _, number = self.audio_stream()
        objs = []
        for k in self.packets:
            for pl in k.payloads:
                if pl.stream != number:
                    continue
                if pl.off == 0:
                    size, pt = struct.unpack_from("<II", pl.rep)
                    objs.append((size, pt, []))
                objs[-1][2].append(pl)
        return objs

    def set_audio_format(self, type_data, block):
        """Replace the WAVEFORMATEX of the audio stream; the 'spread' error correction follows the block size."""
        o, _ = self.audio_stream()
        tsd, ecd = struct.unpack_from("<II", o, 64)
        ec = bytearray(o[78 + tsd:78 + tsd + ecd])
        if ecd >= 7:
            struct.pack_into("<HH", ec, 1, block, block)
        new = o[:78] + type_data + ec + o[78 + tsd + ecd:]
        struct.pack_into("<Q", new, 16, len(new))
        struct.pack_into("<I", new, 64, len(type_data))
        o[:] = new

    def replace_audio(self, contents):
        """`contents`: one byte string per original audio object, at most as large as it."""
        original = self.audio_objects()
        if len(contents) != len(original):
            raise ValueError(f"{len(contents)} audio objects for {len(original)} in the original")
        for (size, pt, pieces), content in zip(original, contents):
            if len(content) > size:
                raise ValueError(f"audio object of {len(content)} bytes for {size} originally")
            # pieces shortened in proportion, each at least 1 byte long
            bounds = [round(sum(len(m.data) for m in pieces[:i]) * len(content) / size)
                      for i in range(len(pieces) + 1)]
            for i in range(1, len(bounds)):
                bounds[i] = max(bounds[i], bounds[i - 1] + 1)
            bounds[-1] = len(content)
            for m, a, b in zip(pieces, bounds, bounds[1:]):
                m.off = a
                m.data = content[a:b]
                m.rep = struct.pack("<II", len(content), pt) + m.rep[8:]

    def to_bytes(self):
        head = bytearray(b"".join(self.objects))
        out = bytearray(HEADER.bytes_le) + struct.pack("<QI", 30 + len(head), len(self.objects))
        out += self.header_reserved + head + self.data_head
        for k in self.packets:
            out += self._write_packet(k)
        return bytes(out + self.trailer)
