"""In-game speech index (SOUND\\SPEECH\\COPSPEECH_xx.IDX) and its Xbox -> Xbox 360 conversion.

Layout (found by comparing the French Xbox index, little endian, with the English 360 one, big
endian; rebuilding both from their own streams gives back the original bytes):

  0x00  u32  version (1)
  0x04  u32  bank count N
  0x54  u32  N (again)
  0x58  N x 16 bytes: u32 type|number (0x01000000 + i), u32 sub-table size,
                      u32 sub-table offset in the IDX, u32 bank offset in the .BIG
  then one sub-table per bank:
  +0    u16  identifier
  +2    u16  parameter (0xFFFF or a small number)
  +4    u8   list format (0 to 3)
  +5    u8   list element count
  +6    u8
  +7    u8   alignment mask m: unit of the list offsets and of the bank size = 256 * (m + 1) bytes
             (0, 1 and 3 on the discs: 256, 512, 1024)
  +8    u16  bank size in units (the game reads the bank with it)
  +10   u16
  +12   list, big endian on both consoles; each element starts with the offset (u16, signed range:
        EA's tool stops at 32767) of its line in the bank, in units:
        format 0: u16; format 1: u16 + u8; format 2: u16 + u16; format 3: u16 + u16 + u8
  then  0x70 padding to a multiple of 4; as many u32 as the format number (in the console's byte
        order: 3, 0x1FF, 0xFFFFF... on both discs); then one byte and a bit mask (0xFF...), the same
        on both consoles.

Byte order changes in: the header, the table, the sub-table headers and the u32 after the lists.
"""
import struct

FORMATS = {0: 2, 1: 3, 2: 4, 3: 5}  # bytes per list element


def read(idx, byte_order):
    e = byte_order
    n = struct.unpack_from(e + "I", idx, 4)[0]
    banks = []
    for i in range(n):
        kind, size, pos, big = struct.unpack_from(e + "4I", idx, 0x58 + 16 * i)
        sub = idx[pos:pos + size]
        fmt, count = sub[4], sub[5]
        width = FORMATS.get(fmt)
        offsets = []
        if width:
            for k in range(count):
                offsets.append(struct.unpack_from(">H", sub, 12 + width * k)[0])
        banks.append(dict(type=kind, size=size, pos=pos, big=big, format=fmt, offsets=offsets))
    return banks


def to_360(idx):
    """Convert an Xbox IDX (little endian) to the Xbox 360 byte order."""
    out = bytearray(idx)

    def swap(fmt, pos):
        out[pos:pos + struct.calcsize(fmt)] = struct.pack(">" + fmt, *struct.unpack_from("<" + fmt, idx, pos))

    n = struct.unpack_from("<I", idx, 4)[0]
    swap("I", 0)
    swap("I", 4)
    swap("I", 0x54)
    for i in range(n):
        base = 0x58 + 16 * i
        swap("4I", base)
        _, _, pos, _ = struct.unpack_from("<4I", idx, base)
        swap("H", pos)
        swap("H", pos + 2)
        swap("H", pos + 8)
        swap("H", pos + 10)
        fmt, count = idx[pos + 4], idx[pos + 5]
        after = pos + (12 + FORMATS[fmt] * count + 3) // 4 * 4
        for k in range(fmt):
            swap("I", after + 4 * k)
    return bytes(out)


def unit(idx, sub_table):
    """Unit (bytes) of the offsets and size of a bank, from its mask (+7)."""
    return 256 * (idx[sub_table + 7] + 1)
