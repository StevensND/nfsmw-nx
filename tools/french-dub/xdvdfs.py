"""Read-only access to Xbox / Xbox 360 disc images (XDVDFS file system)."""
import struct

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Game partition start: plain XISO, original Xbox (redump), Xbox 360 XGD2, XGD3
PARTITION_STARTS = [0, 0x18300000, 0xFD90000, 0x2080000]


def _find_partition(f):
    for base in PARTITION_STARTS:
        f.seek(base + 32 * SECTOR)
        if f.read(len(MAGIC)) == MAGIC:
            return base
    raise SystemExit("XDVDFS file system not found")


def _walk(f, base, sector, size, prefix, out):
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    seen = set()
    stack = [0]
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(data):
            continue
        seen.add(off)
        left, right, start, length, attr, name_len = struct.unpack_from("<HHIIBB", data, off)
        if left == 0xFFFF and right == 0xFFFF:
            continue
        name = data[off + 14:off + 14 + name_len].decode("latin-1")
        if attr & 0x10:
            if length:
                _walk(f, base, start, length, prefix + name + "/", out)
        else:
            out[(prefix + name).upper()] = (length, base + start * SECTOR)
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)


def list_files(iso):
    """{upper-case path: (size, absolute offset in the image)}"""
    with open(iso, "rb") as f:
        base = _find_partition(f)
        f.seek(base + 32 * SECTOR + 20)
        root_sector, root_size = struct.unpack("<II", f.read(8))
        out = {}
        _walk(f, base, root_sector, root_size, "", out)
    return out
